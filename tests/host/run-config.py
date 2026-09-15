"""Build the config_json host test, run it once per fixture and check the log and the file it leaves.

Usage: python tests/host/run-config.py
"""

import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = HERE / "config-host.yaml"
FIXTURES = HERE / "fixtures-config"
STORAGE = HERE / ".storage"
SETTINGS_FILE = STORAGE / "config" / "test.json"
BUILD_SRC = HERE / ".esphome" / "build" / "config-host" / "src"
BINARY = (
    HERE / ".esphome" / "build" / "config-host" / ".pioenvs" / "config-host" / "program"
)
RUN_TIMEOUT_S = 30
ANSI = re.compile(r"\x1b\[[0-9;]*m")

# What the scenario writes in every case: sw_b first (new), then sw_a.
WRITTEN = [
    {"source_name": "sw_b", "inverted": True, "level": 42},
    {"source_name": "sw_a", "inverted": False, "level": 7},
]
# Reloaded after that write when the fixture contributed nothing.
RELOADED = "n=2 sw_b=1/42/v1 sw_a=0/7/v1"
REST = "bad=0 ok=1"

# fixture: the file seeded as config/test.json, or None for "no file at all".
# load/reload/rest: the CHECK payload, bytes stripped off. bytes: size of test.json when the
# scenario re-stats it with nothing dirty — unchanged means the loader did not rewrite it.
CASES = {
    "missing": {
        "fixture": None,
        "load": "n=0",
        "bytes": -1,
        "applied": [],
        "reload": RELOADED,
        "records": WRITTEN,
        "log": ["does not exist, starting with empty settings"],
    },
    "valid": {
        "fixture": "valid.json",
        "load": "n=3 sw_a=1/7/v1 ghost=1/1/v1 sw_b=0/2/v1",
        "bytes": 312,
        "applied": ["sw_a found=1", "ghost found=0", "sw_b found=1"],
        "reload": "n=3 sw_a=0/7/v1 ghost=1/1/v1 sw_b=1/42/v1",
        "records": [
            {"source_name": "sw_a", "inverted": False, "level": 7},
            {"source_name": "ghost", "inverted": True, "level": 1},
            {"source_name": "sw_b", "inverted": True, "level": 42},
        ],
        "log": [],
    },
    "corrupt": {
        "fixture": "corrupt.json",
        "load": "n=0",
        "bytes": 65,
        "applied": [],
        "reload": RELOADED,
        "records": WRITTEN,
        "log": ["Failed to parse JSON"],
    },
    "empty": {
        "fixture": "empty.json",
        "load": "n=0",
        "bytes": 0,
        "applied": [],
        "reload": RELOADED,
        "records": WRITTEN,
        "log": ["Invalid file size: 0 bytes"],
    },
    "notobject": {
        "fixture": "notobject.json",
        "load": "n=0",
        "bytes": 10,
        "applied": [],
        "reload": RELOADED,
        "records": WRITTEN,
        "log": ["Failed to parse JSON"],
    },
    "badrecords": {
        "fixture": "badrecords.json",
        "load": "n=0",
        "bytes": 42,
        "applied": [],
        "reload": RELOADED,
        "records": WRITTEN,
        "log": ["Invalid 'records' field", "Failed to parse test settings"],
    },
    "version9": {
        "fixture": "version9.json",
        "load": "n=1 sw_a=1/3/v9",
        "bytes": 83,
        "applied": ["sw_a found=1"],
        "reload": "n=2 sw_a=0/7/v1 sw_b=1/42/v1",
        "records": [
            {"source_name": "sw_a", "inverted": False, "level": 7},
            {"source_name": "sw_b", "inverted": True, "level": 42},
        ],
        "log": [],
    },
}


def prepare_storage(fixture: str | None) -> None:
    shutil.rmtree(STORAGE, ignore_errors=True)
    SETTINGS_FILE.parent.mkdir(parents=True)
    if fixture is not None:
        shutil.copy(FIXTURES / fixture, SETTINGS_FILE)


def compile_config() -> None:
    subprocess.run(
        [sys.executable, "-m", "esphome", "compile", str(CONFIG)], check=True, cwd=HERE
    )


def check_component_budget() -> list[str]:
    """App.components_ is a StaticVector sized by ESPHOME_COMPONENT_COUNT and push_back past
    capacity drops silently, so registering more than the count leaves components un-set-up."""
    main_cpp = (BUILD_SRC / "main.cpp").read_text()
    defines = (BUILD_SRC / "esphome" / "core" / "defines.h").read_text()
    match = re.search(r"#define ESPHOME_COMPONENT_COUNT (\d+)", defines)
    if match is None:
        return ["no ESPHOME_COMPONENT_COUNT in the generated defines.h"]
    capacity = int(match.group(1))
    registered = main_cpp.count("App.register_component_(")
    print(f"  components: {registered} registered, capacity {capacity}")
    if registered > capacity:
        return [
            f"budget: {registered} App.register_component_ calls exceed "
            f"ESPHOME_COMPONENT_COUNT {capacity}; the last "
            f"{registered - capacity} would be dropped"
        ]
    return []


def run_binary() -> str:
    proc = subprocess.Popen(
        [str(BINARY)],
        cwd=HERE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    lines: list[str] = []
    deadline = time.monotonic() + RUN_TIMEOUT_S
    try:
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            line = ANSI.sub("", line.rstrip("\n"))
            lines.append(line)
            if "DONE" in line:
                break
    finally:
        proc.kill()
        proc.wait()
    return "\n".join(lines)


def checks_of(log: str) -> dict[str, str]:
    return {
        m.group(1): m.group(2).strip()
        for m in re.finditer(r"CHECK (\w+) (.*)$", log, re.M)
    }


def check(name: str, case: dict, log: str) -> list[str]:
    failures: list[str] = []

    def fail(msg: str) -> None:
        failures.append(f"{name}: {msg}")

    if "DONE" not in log:
        fail("scenario did not finish")
        return failures

    got = checks_of(log)
    expected_load = f"{case['load']} bytes={case['bytes']}".strip()
    if got.get("load") != expected_load:
        fail(f"load {got.get('load')!r} expected {expected_load!r}")
    # Nothing is dirty right after a load, so the file must be byte-for-byte what it was.
    if got.get("clean") != f"bytes={case['bytes']}":
        fail(f"clean {got.get('clean')!r} expected 'bytes={case['bytes']}'")
    if got.get("pending") != "p=1":
        fail(f"pending {got.get('pending')!r} expected 'p=1'")
    if not (got.get("saved") or "").startswith("p=0 bytes="):
        fail(f"saved {got.get('saved')!r} expected 'p=0 bytes=...'")
    if got.get("reload") != f"ok=1 {case['reload']}":
        fail(f"reload {got.get('reload')!r} expected 'ok=1 {case['reload']}'")
    if not (got.get("rest") or "").startswith(REST):
        fail(f"rest {got.get('rest')!r} expected to start with {REST!r}")

    ran = re.findall(r"APPLY RAN n=(\d+)$", log, re.M)
    # One line per boot, whatever the fixture held: the apply component made it into
    # App::components_ and setup() reached it.
    if ran != [str(len(case["applied"]))]:
        fail(f"apply ran {ran} expected {[str(len(case['applied']))]}")
    applied = re.findall(r"APPLY (?!RAN)(.*)$", log, re.M)
    if applied != case["applied"]:
        fail(f"applied {applied} expected {case['applied']}")
    # The keeper loads at HARDWARE+5, the apply components run at HARDWARE+1.
    if applied and log.index("APPLY RAN") < log.index("Loaded test settings"):
        fail("apply ran before the keeper loaded the file")
    # Two saves inside save_delay collapse into one write.
    writes = log.count("Saved test settings")
    if writes != 1:
        fail(f"{writes} writes, expected 1")

    for needle in case["log"]:
        if needle not in log:
            fail(f"missing log line {needle!r}")

    if not SETTINGS_FILE.exists():
        fail("no settings file after the run")
        return failures
    data = json.loads(SETTINGS_FILE.read_text())
    if data.get("version") != 1:
        fail(f"file version {data.get('version')!r} expected 1")
    if data.get("records") != case["records"]:
        fail(f"file records {data.get('records')} expected {case['records']}")
    return failures


def main() -> int:
    compile_config()
    if not BINARY.exists():
        print(f"binary not found: {BINARY}")
        return 1
    failures: list[str] = check_component_budget()
    log_path = HERE / "last-config-run.log"
    logs: list[str] = []
    for name, case in CASES.items():
        prepare_storage(case["fixture"])
        log = run_binary()
        logs.append(f"===== {name} =====\n{log}")
        case_failures = check(name, case, log)
        print(f"  {'FAIL' if case_failures else 'ok  '}  {name}")
        failures += case_failures
    log_path.write_text("\n".join(logs))
    if failures:
        print("FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"PASS ({len(CASES)} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
