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


def has_tests(component: Path) -> bool:
    """A suite is a host build, Python tests, or both."""
    return (component / "test.yaml").is_file() or any(component.glob("test_*.py"))


def run_python_tests(component: Path) -> bool:
    """Schema and validator tests next to the cases, if the suite has any."""
    if not any(component.glob("test_*.py")):
        return True
    proc = subprocess.run(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            str(component),
            "-p",
            "test_*.py",
        ],
        cwd=HERE.parent,
    )
    return proc.returncode == 0


def run_component(component: Path, gtest_args: list[str]) -> bool:
    print(f"=== {component.name}", flush=True)
    if not run_python_tests(component):
        return False
    config = component / "test.yaml"
    if not config.is_file():
        # A component the host platform cannot build is tested from Python alone.
        return True
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
    # A report must fail the run, not just print.
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    # The harness main.cpp calls InitGoogleTest() without argv, so flags go in as variables:
    # --gtest_filter=X becomes GTEST_FILTER=X.
    for arg in gtest_args:
        name, _, value = arg.lstrip("-").partition("=")
        env[name.upper()] = value or "1"
    proc = subprocess.run([str(binary_for(config))], cwd=component, env=env)
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
        components = sorted(p for p in COMPONENTS.iterdir() if has_tests(p))
    missing = [c.name for c in components if not has_tests(c)]
    if missing:
        print(f"no tests under tests/components/ for: {', '.join(missing)}")
        return 2
    failed = [c.name for c in components if not run_component(c, gtest_args)]
    if failed:
        print(f"FAILED: {', '.join(failed)}")
        return 1
    print(f"PASSED: {', '.join(c.name for c in components)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
