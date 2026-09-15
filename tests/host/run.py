"""Build the host test config, run it against fresh fixtures and check the log. Usage: python tests/host/run.py"""

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = HERE / "automations-host.yaml"
STORAGE = HERE / ".storage"
RULES = STORAGE / "automations"
PREFS = HERE / ".prefs"
BINARY = (
    HERE
    / ".esphome"
    / "build"
    / "automations-host"
    / ".pioenvs"
    / "automations-host"
    / "program"
)
RUN_TIMEOUT_S = 60
ANSI = re.compile(r"\x1b\[[0-9;]*m")
# The logger puts a timestamp in front of the level.
ERROR_LINE = re.compile(r"^(?:\[[\d:]+\])?\[E\]")

# Relay states at each CHECK label of the first pass.
EXPECTED = {
    "boot": {"r11": 1},
    "input": {"r1": 1, "r2": 1, "r10": 0},
    "hot": {"r3": 1},
    "cooled": {"r3": 0},
    "follow_on": {"r4": 1, "r5": 1, "r6": 0},
    "follow_off": {"r4": 0, "r5": 0, "r6": 1},
    "cond_else": {"r7": 0, "r8": 1},
    "cond_then": {"r7": 1, "r8": 1},
    "cond_and_then": {"r19": 1, "r20": 0},
    "cond_and_else": {"r19": 0, "r20": 1},
    # The restart cancelled the first run's delay, so relay 14 is toggled once.
    "retrigger_mid": {"r13": 1, "r14": 0},
    "retrigger_done": {"r13": 1, "r14": 1},
    # Disabling the rule mid-delay drops the pending action.
    "disable_mid": {"r15": 1, "r16": 0},
    # Two parallel runs, so relay 17 is toggled twice.
    "parallel_mid": {"r17": 1},
    "parallel_done": {"r17": 0},
    "dyn_add": {"r9": 1, "r10": 0},
    "dyn_update": {"r9": 0, "r10": 1},
    "dyn_remove": {"r9": 0, "r10": 0},
    "final": {"r3": 0, "r11": 1},
}

# Relay states after the restart: the rules came back from disk.
EXPECTED_RESTART = {
    "reboot": {"r1": 1, "r2": 0, "r11": 1, "r15": 0, "r16": 0},
}

# id, name and enabled of every rule the second pass loads, in id order. Rule 10
# was disabled through the API in the first pass; rule 12 came from zzz.json.
EXPECTED_RULES = [
    "RULE id=1 name=Input press enabled=1",
    "RULE id=2 name=Click enabled=1",
    "RULE id=3 name=Hot enabled=1",
    "RULE id=4 name=Follow enabled=1",
    "RULE id=5 name=Cond enabled=1",
    "RULE id=6 name=Boot enabled=1",
    "RULE id=7 name=Tick enabled=1",
    "RULE id=8 name=Disabled enabled=0",
    "RULE id=9 name=Retrigger enabled=1",
    "RULE id=10 name=Delayed enabled=0",
    "RULE id=11 name=Parallel enabled=1",
    "RULE id=12 name=Cron Step enabled=1",
    "RULE id=13 name=Composite enabled=1",
    "RULE id=14 name=Orphan enabled=1",
]

# Files that never parse, so the loader leaves them alone.
UNREADABLE = ["badcron.json", "broken.json"]
# Every rule ends up in the file its name maps to: zzz.json becomes cron_step.json.
LOADED_FILES = [
    "boot.json",
    "click.json",
    "composite.json",
    "cond.json",
    "cron_step.json",
    "delayed.json",
    "disabled.json",
    "follow.json",
    "hot.json",
    "input_press.json",
    "orphan.json",
    "parallel.json",
    "retrigger.json",
    "tick.json",
]

# The [E] lines the fixtures are meant to produce. Each must appear, and no other
# [E] line may.
REQUIRED_ERRORS = [
    # broken.json is truncated JSON
    r"JSON parse error in .*broken\.json",
    # badcron.json has a seconds field no second matches
    r"Invalid cron '99 \* \* \* \* \*': a field matches no value",
    r"Failed to load a trigger from the 'triggers' array",
    r"Failed to deserialize automation from .*badcron\.json",
    # orphan.json names an input that does not exist
    r"Trigger: binary sensor 0x[0-9A-F]{8} not found",
    r"Automation 'Orphan': trigger cannot be built",
]


def prepare_storage() -> None:
    shutil.rmtree(STORAGE, ignore_errors=True)
    shutil.rmtree(PREFS, ignore_errors=True)
    RULES.mkdir(parents=True)
    for fixture in (HERE / "fixtures").glob("*.json"):
        shutil.copy(fixture, RULES / fixture.name)


def compile_config() -> None:
    subprocess.run(
        [sys.executable, "-m", "esphome", "compile", str(CONFIG)], check=True, cwd=HERE
    )


def run_binary(phase: str) -> str:
    env = os.environ.copy()
    env["HOST_TEST_PHASE"] = phase
    # Keep the host preferences out of ~/.esphome, where another checkout of this
    # repo would share them.
    env["ESPHOME_PREFDIR"] = str(PREFS)
    proc = subprocess.Popen(
        [str(BINARY)],
        cwd=HERE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
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


def check_relays(log: str, expected: dict[str, dict[str, int]]) -> list[str]:
    failures: list[str] = []
    checks: dict[str, dict[str, int]] = {}
    for match in re.finditer(r"CHECK (\w+) (.*)$", log, re.M):
        checks[match.group(1)] = {
            k: int(v) for k, v in re.findall(r"(r\d+)=(\d)", match.group(2))
        }
    for label, wanted in expected.items():
        got = checks.get(label)
        if got is None:
            failures.append(f"{label}: no CHECK line")
            continue
        for key, value in wanted.items():
            if got.get(key) != value:
                failures.append(f"{label}: {key}={got.get(key)} expected {value}")
    return failures


def check_errors(log: str, phase: str) -> list[str]:
    failures: list[str] = []
    errors = [line for line in log.splitlines() if ERROR_LINE.match(line)]
    for pattern in REQUIRED_ERRORS:
        if not any(re.search(pattern, line) for line in errors):
            failures.append(f"{phase}: expected an error line matching {pattern}")
    unexpected = [
        line
        for line in errors
        if not any(re.search(pattern, line) for pattern in REQUIRED_ERRORS)
    ]
    if unexpected:
        failures.append(f"{phase}: unexpected errors: " + "; ".join(unexpected[:5]))
    return failures


def check_first_pass(log: str) -> list[str]:
    failures = check_relays(log, EXPECTED)
    failures += check_errors(log, "first pass")

    if not re.search(r"ADD id=[1-9]\d* taken=1", log):
        failures.append("ADD: no id assigned or duplicate name not detected")
    for step in ("UPDATE ok=1", "REMOVE ok=1", "DISABLE ok=1"):
        if step not in log:
            failures.append(f"{step} missing")
    for name, least in (("Tick", 2), ("Cron Step", 2)):
        fired = log.count(f"Automation '{name}' is triggered")
        if fired < least:
            failures.append(f"cron '{name}' fired {fired} times, expected {least}+")
    if log.count("Automation 'Hot' is triggered") != 1:
        failures.append("temperature trigger must fire exactly once per crossing")
    for name, times in (("Retrigger", 2), ("Parallel", 2)):
        fired = log.count(f"Automation '{name}' is triggered")
        if fired != times:
            failures.append(f"'{name}' fired {fired} times, expected {times}")
    if "Automation 'Disabled' is triggered" in log:
        failures.append("disabled automation fired")

    files = sorted(p.name for p in RULES.glob("*.json"))
    expected_files = sorted(LOADED_FILES + UNREADABLE)
    if files != expected_files:
        failures.append(f"storage files {files} expected {expected_files}")
    for name in LOADED_FILES:
        path = RULES / name
        if not path.exists():
            continue
        data = json.loads(path.read_text())
        if data.get("id", 0) == 0:
            failures.append(f"{name}: id not persisted")
    failures += check_stored_rules()
    return failures


def check_stored_rules() -> list[str]:
    """The two rules the first pass rewrote, read back from disk."""
    failures: list[str] = []
    renamed = RULES / "cron_step.json"
    if not renamed.exists():
        failures.append("cron_step.json: the rule in zzz.json was not moved")
    else:
        data = json.loads(renamed.read_text())
        cron = data["triggers"][0].get("cron")
        if cron != "*/2 * * * * *":
            failures.append(f"cron_step.json: cron {cron!r} expected '*/2 * * * * *'")
        if data.get("name") != "Cron Step":
            failures.append(f"cron_step.json: name {data.get('name')!r}")
    disabled = RULES / "delayed.json"
    if (
        disabled.exists()
        and json.loads(disabled.read_text()).get("enabled") is not False
    ):
        failures.append("delayed.json: set_enable_automation(false) was not persisted")
    for name in UNREADABLE:
        if (RULES / name).read_bytes() != (HERE / "fixtures" / name).read_bytes():
            failures.append(f"{name}: a file that failed to load was rewritten")
    return failures


def check_second_pass(log: str) -> list[str]:
    failures = check_relays(log, EXPECTED_RESTART)
    failures += check_errors(log, "second pass")

    rules = re.findall(r"RULE id=.*$", log, re.M)
    if rules != EXPECTED_RULES:
        failures.append(f"reloaded rules {rules} expected {EXPECTED_RULES}")
    if "RESET left=0" not in log:
        failures.append("reset_all did not empty the rule list")

    files = sorted(p.name for p in RULES.glob("*.json"))
    if files != sorted(UNREADABLE):
        failures.append(f"after reset_all {files} expected {sorted(UNREADABLE)}")
    return failures


def main() -> int:
    prepare_storage()
    compile_config()
    if not BINARY.exists():
        print(f"binary not found: {BINARY}")
        return 1

    failures: list[str] = []
    for phase, checker in (("1", check_first_pass), ("2", check_second_pass)):
        log = run_binary(phase)
        (HERE / f"last-run-{phase}.log").write_text(log)
        if "DONE" not in log:
            failures.append(f"pass {phase}: the run did not finish")
            break
        failures += checker(log)

    if failures:
        print("FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
