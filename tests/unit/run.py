"""Build the automations unit tests for the host and run them. Usage: python tests/unit/run.py [gtest args]"""

import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = HERE / "automations-unit.yaml"
BINARY = (
    HERE
    / ".esphome"
    / "build"
    / "automations-unit"
    / ".pioenvs"
    / "automations-unit"
    / "program"
)


def main() -> int:
    subprocess.run(
        [sys.executable, "-m", "esphome", "compile", str(CONFIG)], check=True, cwd=HERE
    )
    env = os.environ.copy()
    # Keep the host preferences out of ~/.esphome, shared with other checkouts.
    env["ESPHOME_PREFDIR"] = str(HERE / ".prefs")
    # App and the test entities live for the whole run; upstream runs its tests the same way.
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    return subprocess.run([str(BINARY), *sys.argv[1:]], cwd=HERE, env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
