#!/usr/bin/env python3
"""Smoke check of the thermostats on a JXD firmware booted in QEMU.

Boots the device with scripts/qemu.sh, creates a thermostat with presets through the climate
editor's HTTP API, and checks from the native API, as Home Assistant sees it, that:

- the built-in and custom presets are listed;
- a pick applies the preset's target and mode, and a preset that keeps the mode leaves it be;
- a values-only edit of the presets causes no reconnect;
- the active preset survives a restart.

It prints one line per check, stops at the first failure with a non-zero exit, and deletes the
thermostat and stops the emulator on the way out. Needs only what ESPHome pulls in
(aioesphomeapi, requests), so the repo venv runs it as is:

    .venv/bin/python scripts/qemu-climate-smoke.py
    .venv/bin/python scripts/qemu-climate-smoke.py jxd-r6-e1eth --no-build
"""

from __future__ import annotations

import argparse
import asyncio
from collections.abc import Callable
import contextlib
from functools import partial
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from typing import TypeVar

from aioesphomeapi import (
    APIClient,
    APIConnectionError,
    ClimateInfo,
    ClimateMode,
    ClimatePreset,
    ClimateState,
    EncryptionPlaintextAPIError,
    InvalidAuthAPIError,
    InvalidEncryptionKeyAPIError,
    RequiresEncryptionAPIError,
)
import requests
from requests.auth import HTTPDigestAuth

ROOT = Path(__file__).resolve().parent.parent
QEMU_SH = ROOT / "scripts" / "qemu.sh"
USER_ENV = "DEVICE_USER"
API_KEY_ENV = "DEVICE_API_KEY"

THERMOSTAT_ID = "qemu-smoke"
THERMOSTAT_NAME = "QEMU Smoke"
# packages/qemu/climate-plant.yaml: the room that Relay 1 warms.
SENSOR_ID = "qemu_room_temperature"
# Built-in presets by their names on the device, the custom one by its own.
ECO, COMFORT, CUSTOM = "Eco", "Comfort", "Frost Guard"
CUSTOM_KEY = "frost-guard"

# The thermostat starts in heat at 20; each preset moves the target, and two of them the mode.
THERMOSTAT = {
    "name": THERMOSTAT_NAME,
    "enabled": True,
    "kind": "bang_bang",
    "sensor_id": SENSOR_ID,
    "mode": "heat",
    "setpoint": 20,
    "presets": [
        {"name": ECO, "setpoint": 17, "mode": "keep"},
        {"name": COMFORT, "setpoint": 22.5, "mode": "heat"},
        {"name": CUSTOM, "setpoint": 8, "mode": "off"},
    ],
}

# The hub asks API clients to reconnect 2 s after a change that needs it; QEMU adds to that.
RECONNECT_WAIT_S = 16.0
# A no-reconnect check waits this long at least, and twice what the control's drop took.
QUIET_WINDOW_S = 8.0
STATE_TIMEOUT_S = 20.0
# LittleFS under QEMU is slow: a save takes seconds.
HTTP_TIMEOUT_S = 60.0
FILE_TIMEOUT_S = 30.0
# Longer than the 5 s a route waits for a busy loop before it answers 503.
PROBE_TIMEOUT_S = 8.0
# QEMU now and then hangs right after the bootloader; a second cold start gets through.
BOOT_ATTEMPTS = 3

T = TypeVar("T")


class CheckFailed(Exception):
    """The first failed check, or a step without which nothing can be checked."""


def ok(text: str) -> None:
    print(f"ok    {text}", flush=True)


def note(text: str) -> None:
    print(f"==    {text}", flush=True)


def warn(text: str) -> None:
    print(f"warn  {text}", file=sys.stderr, flush=True)


def same(a: float, b: float) -> bool:
    return abs(a - b) < 0.01


def describe(state: ClimateState | None) -> str:
    if state is None:
        return "no state received"
    preset = state.custom_preset or (state.preset.name if state.preset else "NONE")
    mode = state.mode.name if state.mode is not None else "?"
    return f"target {state.target_temperature:g}, mode {mode}, preset {preset}"


def active_of(row: dict) -> str:
    """A list row's active preset, for a failure message."""
    if not row["active_preset"]:
        return "no active preset"
    return f'"{row["active_preset_name"]}" ({row["active_preset"]}) active'


# --- the emulator ------------------------------------------------------------


class Emulator:
    """scripts/qemu.sh for one device on one trio of ports."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.device = args.device
        self.ports = [
            "--http-port",
            str(args.http_port),
            "--api-port",
            str(args.api_port),
            "--ota-port",
            str(args.ota_port),
        ]
        self.verbose = args.verbose
        self.started = False

    async def _qemu_sh(self, *argv: str) -> None:
        pipe = None if self.verbose else asyncio.subprocess.PIPE
        proc = await asyncio.create_subprocess_exec(
            str(QEMU_SH),
            *argv,
            cwd=ROOT,
            stdout=pipe,
            stderr=None if self.verbose else asyncio.subprocess.STDOUT,
        )
        try:
            output, _ = await proc.communicate()
        except asyncio.CancelledError:
            # A SIGTERM reaches this process alone: take a compile or a start down with it.
            with contextlib.suppress(ProcessLookupError):
                proc.terminate()
            await proc.wait()
            raise
        if proc.returncode != 0:
            text = output.decode(errors="replace") if output else ""
            tail = "\n".join(text.splitlines()[-30:])
            raise CheckFailed(
                f"scripts/qemu.sh {' '.join(argv)} exited {proc.returncode}"
                + (f"\n{tail}" if tail else "")
            )

    async def start(self, build: bool, fresh: bool) -> None:
        """Starts the emulator and returns; whether the firmware comes up is the caller's to see."""
        # `run` itself compiles, so its port check and old-instance stop come before the compile.
        argv = ["run", self.device, "--daemon", *self.ports]
        if not build:
            argv.append("--no-build")
        if fresh:
            argv.append("--fresh")
        self.started = True
        await self._qemu_sh(*argv)

    async def stop(self) -> None:
        await self._qemu_sh("stop", self.device)

    def stop_now(self) -> None:
        """Blocking, for the way out: nothing a signal cancels on the loop can skip it."""
        result = subprocess.run(
            [str(QEMU_SH), "stop", self.device],
            cwd=ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if self.verbose or result.returncode != 0:
            # The terminal may be gone (SIGHUP); the emulator is stopped all the same.
            with contextlib.suppress(OSError):
                sys.stdout.write(result.stdout)
                if result.returncode != 0:
                    warn(f"scripts/qemu.sh stop exited {result.returncode}")

    def log_path(self) -> Path | None:
        found = sorted(
            ROOT.glob(f"devices/*/.esphome/build/{self.device}-qemu/qemu.log")
        )
        return found[0] if found else None

    def log(self) -> str:
        path = self.log_path()
        return str(path.relative_to(ROOT)) if path else "the emulator's qemu.log"

    def in_safe_mode(self) -> bool:
        """Whether this start came up in safe mode, which serves no web server."""
        path = self.log_path()
        try:
            return path is not None and "SAFE MODE IS ACTIVE" in path.read_text(
                errors="replace"
            )
        except OSError:
            return False

    def keep_log(self, number: int) -> str | None:
        """Copies qemu.log aside as qemu.log.<number>: the next `run` truncates it."""
        path = self.log_path()
        if path is None:
            return None
        kept = path.with_name(f"{path.name}.{number}")
        shutil.copyfile(path, kept)
        return str(kept.relative_to(ROOT))


# --- the climate editor's HTTP API --------------------------------------------


class Worker:
    """One daemon thread that makes every HTTP request.

    One thread: Digest keeps its challenge per thread, and a POST sent without one gets a 401
    that leaves its body unread and the connection out of step. A daemon: an interrupted run
    need not wait out a request in flight.
    """

    def __init__(self) -> None:
        self.jobs: queue.SimpleQueue = queue.SimpleQueue()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        while True:
            loop, future, job = self.jobs.get()
            result, error = None, None
            try:
                result = job()
            except Exception as err:
                error = err
            # The run may be over and its loop closed by now.
            with contextlib.suppress(RuntimeError):
                loop.call_soon_threadsafe(self._settle, future, result, error)

    @staticmethod
    def _settle(
        future: asyncio.Future, result: object, error: Exception | None
    ) -> None:
        if future.done():  # its caller was cancelled
            return
        if error is None:
            future.set_result(result)
        else:
            future.set_exception(error)

    async def run(self, job: Callable[[], T]) -> T:
        loop = asyncio.get_running_loop()
        future = loop.create_future()
        self.jobs.put((loop, future, job))
        return await future


class Editor:
    """The routes under /climate-editor/api/, and the file API for what reached flash."""

    def __init__(self, base: str, user: str, password: str) -> None:
        self.base = base
        self.session = requests.Session()
        self.session.auth = HTTPDigestAuth(user, password)
        self.worker = Worker()

    def _request(
        self, method: str, path: str, params: dict | None, body: object
    ) -> requests.Response:
        headers = {}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        # The loop answers 503 when it was busy for five seconds; nothing changed then.
        for _ in range(5):
            try:
                response = self.session.request(
                    method,
                    self.base + path,
                    params=params,
                    data=data,
                    headers=headers,
                    timeout=HTTP_TIMEOUT_S,
                )
            except requests.RequestException as err:
                raise CheckFailed(f"{method} {path}: {err}") from err
            if response.status_code != 503:
                return response
            time.sleep(2)
        return response

    async def call(
        self, method: str, route: str, params: dict | None = None, body: object = None
    ) -> dict:
        """A route that must succeed: its JSON answer, or CheckFailed with the device's words."""
        return await self.worker.run(partial(self._call, method, route, params, body))

    def _call(self, method: str, route: str, params: dict | None, body: object) -> dict:
        response = self._request(method, "/climate-editor/api/" + route, params, body)
        try:
            answer = response.json()
        except ValueError:
            answer = None
        if response.status_code != 200 or not isinstance(answer, dict):
            raise CheckFailed(
                f"{method} {route}: HTTP {response.status_code} {response.text[:300]}"
            )
        return answer

    async def read_file(self, path: str) -> dict | None:
        return await self.worker.run(partial(self._read_file, path))

    def _read_file(self, path: str) -> dict | None:
        response = self._request("GET", "/files/download", {"path": path}, None)
        if response.status_code != 200:
            return None
        try:
            return response.json()
        except ValueError:
            return None

    async def probe(self) -> int:
        """The list route's status, or 0 when nothing answered."""
        return await self.worker.run(self._probe)

    def _probe(self) -> int:
        try:
            response = self.session.get(
                self.base + "/climate-editor/api/list", timeout=PROBE_TIMEOUT_S
            )
        except requests.RequestException:
            return 0
        if response.status_code == 401:
            raise CheckFailed("the web server refused the credentials (--user)")
        if response.status_code == 404:
            raise CheckFailed("this firmware serves no /climate-editor/api/")
        return response.status_code


# --- the native API, as Home Assistant ----------------------------------------


class Api:
    """One client that reconnects on request and remembers whether the device dropped it."""

    def __init__(self, port: int, noise_psk: str | None) -> None:
        self.client = APIClient(
            "127.0.0.1",
            port,
            None,
            noise_psk=noise_psk,
            client_info="qemu-climate-smoke",
            # The device keeps its own clock: homeassistant time would take this client's.
            provide_time=False,
        )
        self.dropped = asyncio.Event()
        self.dropped_at = 0.0
        self.climates: dict[str, ClimateInfo] = {}
        self.states: dict[int, ClimateState] = {}
        self.connected = False

    async def _on_stop(self, expected_disconnect: bool) -> None:
        self.connected = False
        self.dropped_at = time.monotonic()
        self.dropped.set()

    def _on_state(self, state: object) -> None:
        if isinstance(state, ClimateState):
            self.states[state.key] = state

    async def connect(self, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while True:
            try:
                # Logged by aioesphomeapi too otherwise; the FAIL line carries the last one.
                await self.client.connect(
                    on_stop=self._on_stop, login=True, log_errors=False
                )
                break
            except (
                RequiresEncryptionAPIError,
                InvalidEncryptionKeyAPIError,
                EncryptionPlaintextAPIError,
                InvalidAuthAPIError,
            ) as err:
                # A key that is missing, wrong or not wanted stays so however long we wait.
                raise CheckFailed(
                    f"the native API refused this client (--api-key, ${API_KEY_ENV}):"
                    f" {type(err).__name__}: {err}"
                ) from err
            except APIConnectionError as err:
                if time.monotonic() > deadline:
                    raise CheckFailed(
                        f"the native API did not accept a connection in {timeout:g} s: {err}"
                    ) from err
                await asyncio.sleep(2)
        self.dropped.clear()
        self.connected = True
        entities, _ = await self.client.list_entities_services()
        self.climates = {e.name: e for e in entities if isinstance(e, ClimateInfo)}
        self.states = {}
        self.client.subscribe_states(self._on_state)

    async def disconnect(self) -> None:
        if self.connected:
            self.connected = False
            # A link the device is dropping at that moment is gone either way.
            with contextlib.suppress(APIConnectionError):
                await self.client.disconnect()

    def info(self, name: str) -> ClimateInfo:
        info = self.climates.get(name)
        if info is None:
            raise CheckFailed(
                f'the native API lists no climate "{name}": {sorted(self.climates)}'
            )
        return info

    async def wait_state(
        self, name: str, pred: Callable[[ClimateState], bool]
    ) -> tuple[bool, ClimateState | None]:
        key = self.info(name).key
        deadline = time.monotonic() + STATE_TIMEOUT_S
        while True:
            state = self.states.get(key)
            if state is not None and pred(state):
                return True, state
            if time.monotonic() > deadline:
                return False, state
            await asyncio.sleep(0.1)

    async def wait_dropped(self, timeout: float) -> bool:
        if self.dropped.is_set():
            return True
        try:
            await asyncio.wait_for(self.dropped.wait(), max(timeout, 0))
        except TimeoutError:
            return False
        return True


# --- the checks ---------------------------------------------------------------


class Smoke:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.qemu = Emulator(args)
        user, _, password = args.user.partition(":")
        self.editor = Editor(f"http://127.0.0.1:{args.http_port}", user, password)
        self.api = Api(args.api_port, args.api_key)
        # What a save may have created: cleanup deletes these and anything by our name.
        self.ids = {THERMOSTAT_ID}
        self.created = False
        self.up = False
        self.quiet_window = QUIET_WINDOW_S
        self.checks = 0
        self.retries = 0

    def passed(self, text: str) -> None:
        self.checks += 1
        ok(text)

    async def http(self, method: str, route: str, **kwargs) -> dict:
        return await self.editor.call(method, route, **kwargs)

    async def wait_ready(self, timeout: float) -> int:
        """200 once the thermostats are listed; else the last status seen, 0 for none at all.

        Before the guest network is up slirp accepts and never answers; while setup still
        holds the loop, the routes answer 503.
        """
        deadline = time.monotonic() + timeout
        last = 0
        while time.monotonic() < deadline:
            status = await self.editor.probe()
            if status == 200:
                return status
            # Each start this script kills short of a minute counts as a failed boot.
            if self.qemu.in_safe_mode():
                raise CheckFailed(
                    "the device came up in safe mode, having counted too many short boots;"
                    " run once with --fresh, or without --no-build, to start from a blank flash"
                )
            last = status or last
            await asyncio.sleep(2)
        return last

    async def boot(self, build: bool, fresh: bool) -> None:
        self.up = False
        timeout = self.args.boot_timeout
        for attempt in range(BOOT_ATTEMPTS):
            # Only the first start compiles or wipes the flash: a retry boots what that one left.
            first = attempt == 0
            await self.qemu.start(build and first, fresh and first)
            status = await self.wait_ready(timeout)
            if status == 200:
                self.up = True
                return
            if status:
                raise CheckFailed(
                    f"the device answered HTTP {status} and never listed its thermostats in"
                    f" {timeout} s: the firmware is stuck, not QEMU; see {self.qemu.log()}"
                )
            await self.qemu.stop()
            if attempt + 1 < BOOT_ATTEMPTS:
                self.retries += 1
                kept = self.qemu.keep_log(self.retries)
                note(
                    f"no answer in {timeout} s, QEMU hung (its log is kept as {kept}):"
                    " starting it again"
                )
        raise CheckFailed(
            f"nothing answered in {BOOT_ATTEMPTS} starts of {timeout} s;"
            f" see {self.qemu.log()} and the earlier starts' qemu.log.<n>"
        )

    async def free_relay(self) -> str:
        """A relay no enabled thermostat names: a running one holds it, a waiting one reserves it."""
        entities = await self.http("GET", "entities")
        listed = await self.http("GET", "list")
        if not any(s["object_id"] == SENSOR_ID for s in entities["sensors"]):
            raise CheckFailed(
                f"no sensor {SENSOR_ID}: not a QEMU build with packages/qemu/climate-plant.yaml"
            )
        taken = set()
        for row in listed["controllers"]:
            if row["enabled"]:
                taken.update((row["heat_relay_id"], row["cool_relay_id"]))
        for switch in entities["switches"]:
            relay = switch["object_id"]
            if re.fullmatch(r"relay_\d+", relay) and relay not in taken:
                return relay
        raise CheckFailed("every relay is held or reserved by another thermostat")

    async def remove_ours(self) -> list[str]:
        """Deletes every thermostat listed under our name or an id a save gave; their ids."""
        listed = await self.http("GET", "list")
        ids = [
            row["id"]
            for row in listed["controllers"]
            if row["id"] in self.ids or row["name"] == THERMOSTAT_NAME
        ]
        for id_ in ids:
            answer = await self.http("POST", "delete", params={"id": id_})
            if answer.get("persisted") is False:
                warn(f"{id_} is gone, its file is not: it returns at the next boot")
        return ids

    async def remove_leftover(self) -> None:
        if not await self.remove_ours():
            return
        note(f'removed "{THERMOSTAT_NAME}" left over from an earlier run')
        # A drop the removal asks for lands here, not on create()'s control check.
        if await self.api.wait_dropped(RECONNECT_WAIT_S):
            await self.api.connect(self.args.boot_timeout)

    async def create(self) -> None:
        relay = await self.free_relay()
        doc = dict(THERMOSTAT, heat={"relay_id": relay})
        # Before the request: one that times out may still have created it.
        self.created = True
        sent = time.monotonic()
        answer = await self.http("POST", "save", body=doc)
        if isinstance(answer.get("id"), str):
            self.ids.add(answer["id"])
        if answer.get("id") != THERMOSTAT_ID:
            raise CheckFailed(
                f"create: answered id {answer.get('id')!r}, not {THERMOSTAT_ID}"
            )
        if answer.get("warning"):
            raise CheckFailed(
                f"create: the thermostat did not start: {answer['warning']}"
            )
        note(f'created "{THERMOSTAT_NAME}" on {relay} and {SENSOR_ID}')
        # The control for the no-reconnect check below: a change that needs one gets one.
        if not await self.api.wait_dropped(RECONNECT_WAIT_S):
            raise CheckFailed(
                "create: a new running thermostat did not make the API client reconnect"
                f" in {RECONNECT_WAIT_S:g} s"
            )
        took = self.api.dropped_at - sent
        self.quiet_window = max(QUIET_WINDOW_S, 2 * took)
        await self.api.connect(self.args.boot_timeout)
        self.passed(
            "create: a new running thermostat makes the API client reconnect"
            f" ({took:.1f} s after the save)"
        )

    def check_listed(self) -> None:
        info = self.api.info(THERMOSTAT_NAME)
        built_in = sorted(p.name for p in info.supported_presets)
        custom = list(info.supported_custom_presets)
        if built_in != ["COMFORT", "ECO"] or custom != [CUSTOM]:
            raise CheckFailed(
                f"presets: the API lists built-in {built_in} and custom {custom},"
                f' expected [COMFORT, ECO] and ["{CUSTOM}"]'
            )
        self.passed(
            f'presets: the API lists built-in COMFORT and ECO, custom "{CUSTOM}"'
        )

    async def pick_api(
        self, label: str, target: float, mode: ClimateMode, **preset
    ) -> None:
        key = self.api.info(THERMOSTAT_NAME).key
        self.api.client.climate_command(key, **preset)
        want_preset = preset.get("preset", ClimatePreset.NONE)
        want_custom = preset.get("custom_preset", "")

        def applied(s: ClimateState) -> bool:
            return (
                same(s.target_temperature, target)
                and s.mode == mode
                and (s.preset or ClimatePreset.NONE) == want_preset
                and s.custom_preset == want_custom
            )

        done, state = await self.api.wait_state(THERMOSTAT_NAME, applied)
        if not done:
            raise CheckFailed(
                f"pick {label} over the API: expected target {target:g}, mode {mode.name};"
                f" got {describe(state)}"
            )
        self.passed(f"pick {label} over the API: target {target:g}, mode {mode.name}")

    async def pick_editor(self) -> None:
        """Eco keeps the mode, and the pick before it turned the thermostat off."""
        answer = await self.http(
            "POST", "preset", params={"id": THERMOSTAT_ID, "key": "eco"}
        )
        if answer.get("persisted") is not True:
            raise CheckFailed(f"pick eco in the editor: {answer}")
        done, state = await self.api.wait_state(
            THERMOSTAT_NAME,
            lambda s: same(s.target_temperature, 17)
            and s.mode == ClimateMode.OFF
            and s.preset == ClimatePreset.ECO,
        )
        if not done:
            raise CheckFailed(
                "pick eco in the editor: expected target 17, mode OFF kept, preset ECO;"
                f" got {describe(state)}"
            )
        row = await self.active_row()
        if row["active_preset"] != "eco" or row["active_preset_name"] != ECO:
            raise CheckFailed(
                f"pick eco in the editor: the list shows {active_of(row)}"
            )
        self.passed(
            "pick eco in the editor: target 17, mode OFF kept, listed as active"
        )

    async def active_row(self) -> dict:
        listed = await self.http("GET", "list")
        for row in listed["controllers"]:
            if row["id"] == THERMOSTAT_ID:
                return row
        raise CheckFailed(f"{THERMOSTAT_ID} is not listed: {listed}")

    async def edit_values(self) -> None:
        doc = await self.http("GET", "get", params={"id": THERMOSTAT_ID})
        for preset in doc["presets"]:
            if preset["name"] == ECO:
                preset["setpoint"] = 16.5
            elif preset["name"] == CUSTOM:
                preset["setpoint"] = 9
        sent = time.monotonic()
        await self.http("POST", "save", body=doc)
        done, state = await self.api.wait_state(
            THERMOSTAT_NAME,
            lambda s: same(s.target_temperature, 16.5)
            and s.mode == ClimateMode.OFF
            and s.preset == ClimatePreset.ECO,
        )
        if not done:
            raise CheckFailed(
                "values-only edit: expected the active ECO's new target 16.5, mode OFF;"
                f" got {describe(state)}"
            )
        self.passed(
            "values-only edit: the active preset's new target 16.5 applies at once"
        )
        # Timed from the save, like the control's drop in create().
        window = self.quiet_window
        if await self.api.wait_dropped(window - (time.monotonic() - sent)):
            raise CheckFailed(
                "values-only edit: the device made the API client reconnect"
            )
        # A link that died quietly would pass the wait as well.
        try:
            await asyncio.wait_for(self.api.client.device_info(), 10)
        except (APIConnectionError, TimeoutError) as err:
            raise CheckFailed(f"values-only edit: the API link is gone: {err}") from err
        self.passed(
            f"values-only edit: no API reconnect in {window:.1f} s after the save"
        )

    async def restart(self) -> None:
        await self.pick_api(
            f'"{CUSTOM}" (edited)', 9, ClimateMode.OFF, custom_preset=CUSTOM
        )
        # The pick reaches the file a few seconds later; a stop before that would lose it.
        path = f"/climates/{THERMOSTAT_ID}.json"
        deadline = time.monotonic() + FILE_TIMEOUT_S
        while True:
            stored = await self.editor.read_file(path)
            if stored is not None and stored.get("active_preset") == CUSTOM_KEY:
                break
            if time.monotonic() > deadline:
                found = stored.get("active_preset") if stored else "no file"
                raise CheckFailed(
                    f"restart: {path} never recorded the pick of {CUSTOM_KEY}: {found}"
                )
            await asyncio.sleep(1)
        await self.api.disconnect()
        note("restarting the emulator (stop, then a cold boot of the same flash)")
        await self.qemu.stop()
        await self.boot(build=False, fresh=False)
        await self.api.connect(self.args.boot_timeout)
        row = await self.active_row()
        if row["active_preset"] != CUSTOM_KEY or row["active_preset_name"] != CUSTOM:
            raise CheckFailed(
                f'restart: expected "{CUSTOM}" active, the list shows {active_of(row)}'
            )
        done, state = await self.api.wait_state(
            THERMOSTAT_NAME,
            lambda s: s.custom_preset == CUSTOM
            and same(s.target_temperature, 9)
            and s.mode == ClimateMode.OFF,
        )
        if not done:
            raise CheckFailed(
                f'restart: expected "{CUSTOM}", target 9, mode OFF; got {describe(state)}'
            )
        self.passed(
            f'restart: "{CUSTOM}" is still the active preset, target 9, mode OFF'
        )

    async def cleanup(self) -> None:
        try:
            await self.api.disconnect()
            if self.created and not self.up:
                warn(
                    f'the device is not up to delete "{THERMOSTAT_NAME}": it stays on the'
                    " emulated flash until the next run removes it"
                )
            elif self.created:
                try:
                    await self.remove_ours()
                except CheckFailed as err:
                    warn(
                        f'could not delete "{THERMOSTAT_NAME}", it may still be there: {err}'
                    )
        finally:
            if self.qemu.started and not self.args.keep_running:
                self.qemu.stop_now()

    async def run(self) -> None:
        note(
            f"booting {self.args.device} in QEMU"
            + (" (compiling first)" if self.args.build else "")
        )
        await self.boot(self.args.build, self.args.fresh)
        await self.api.connect(self.args.boot_timeout)
        await self.remove_leftover()
        await self.create()
        self.check_listed()
        await self.pick_api(
            "COMFORT", 22.5, ClimateMode.HEAT, preset=ClimatePreset.COMFORT
        )
        await self.pick_api(f'"{CUSTOM}"', 8, ClimateMode.OFF, custom_preset=CUSTOM)
        await self.pick_editor()
        await self.edit_values()
        await self.restart()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "device",
        nargs="?",
        default="jxd-r6-e1eth-lcd",
        help="device config (default: %(default)s)",
    )
    parser.add_argument(
        "--no-build",
        dest="build",
        action="store_false",
        help="boot what was built last, on the flash the last run left",
    )
    parser.add_argument(
        "--fresh",
        action="store_true",
        help="wipe the emulated flash first; a compile does that anyway",
    )
    # The defaults of scripts/qemu.sh.
    parser.add_argument(
        "--http-port",
        type=int,
        default=8080,
        help="host port for the web server (default: %(default)s)",
    )
    parser.add_argument(
        "--api-port",
        type=int,
        default=6053,
        help="host port for the native API (default: %(default)s)",
    )
    parser.add_argument(
        "--ota-port",
        type=int,
        default=3232,
        help="host port for OTA (default: %(default)s)",
    )
    parser.add_argument(
        "--boot-timeout",
        type=int,
        default=180,
        help="seconds a start may take before it counts as hung, LittleFS included"
        " (default: %(default)s)",
    )
    # The environment's values stay out of --help: a password or a key would show there.
    parser.add_argument(
        "-u",
        "--user",
        metavar="USER:PASSWORD",
        help=f"the web server's credentials; also ${USER_ENV} (default: admin:admin)",
    )
    parser.add_argument(
        "--api-key",
        help="the native API's encryption key, once one was set on the device;"
        f" also ${API_KEY_ENV}, which keeps it out of ps and the shell history",
    )
    parser.add_argument(
        "--keep-running", action="store_true", help="leave the emulator up afterwards"
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="show scripts/qemu.sh's output"
    )
    args = parser.parse_args()
    args.user = args.user or os.environ.get(USER_ENV) or "admin:admin"
    args.api_key = args.api_key or os.environ.get(API_KEY_ENV) or None
    return args


async def main(args: argparse.Namespace) -> int:
    smoke = Smoke(args)
    task = asyncio.current_task()
    caught: list[int] = []

    def interrupted(signum: int) -> None:
        caught.append(signum)
        task.cancel()

    # A kill or a closed terminal still deletes the thermostat and stops the emulator.
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGTERM, signal.SIGHUP):
        loop.add_signal_handler(signum, interrupted, signum)
    try:
        await smoke.run()
    except CheckFailed as err:
        print(f"FAIL  {err}", flush=True)
        return 1
    except APIConnectionError as err:
        print(f"FAIL  the native API: {err}", flush=True)
        return 1
    except asyncio.CancelledError:
        if not caught:
            raise  # Ctrl-C: asyncio.run makes it a KeyboardInterrupt
        return 128 + caught[0]
    finally:
        await smoke.cleanup()
    retries = ""
    if smoke.retries:
        retries = f", after {smoke.retries} boot {'retry' if smoke.retries == 1 else 'retries'}"
        retries += " on a QEMU hang"
    print(f"PASS  {smoke.checks} checks{retries}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main(parse_args())))
    except (KeyboardInterrupt, asyncio.CancelledError):
        print("stopped", file=sys.stderr)
        sys.exit(130)
