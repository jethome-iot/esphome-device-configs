"""Build and run the Google Test binaries under tests/components/. Usage: python tests/run.py [component ...] [-- gtest args]"""

import os
import subprocess
import sys
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
COMPONENTS = HERE / "components"


def binary_for(config: Path) -> Path:
    name = yaml.safe_load(config.read_text())["esphome"]["name"]
    return config.parent / ".esphome" / "build" / name / ".pioenvs" / name / "program"


def run_component(component: Path, gtest_args: list[str]) -> bool:
    config = component / "test.yaml"
    print(f"=== {component.name}", flush=True)
    subprocess.run(
        [sys.executable, "-m", "esphome", "compile", config.name],
        check=True,
        cwd=component,
    )
    env = os.environ.copy()
    # Keep the host preferences out of ~/.esphome, shared with other checkouts.
    env["ESPHOME_PREFDIR"] = str(component / ".prefs")
    # App and the test entities live for the whole run; upstream runs its tests the same way.
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    proc = subprocess.run(
        [str(binary_for(config)), *gtest_args], cwd=component, env=env
    )
    return proc.returncode == 0


def main() -> int:
    args = sys.argv[1:]
    gtest_args: list[str] = []
    if "--" in args:
        split = args.index("--")
        args, gtest_args = args[:split], args[split + 1 :]
    if args:
        components = [COMPONENTS / name for name in args]
    else:
        components = sorted(
            p for p in COMPONENTS.iterdir() if (p / "test.yaml").is_file()
        )
    missing = [c.name for c in components if not (c / "test.yaml").is_file()]
    if missing:
        print(f"no test.yaml under tests/components/ for: {', '.join(missing)}")
        return 2
    failed = [c.name for c in components if not run_component(c, gtest_args)]
    if failed:
        print(f"FAILED: {', '.join(failed)}")
        return 1
    print(f"PASSED: {', '.join(c.name for c in components)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
