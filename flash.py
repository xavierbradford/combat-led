#!/usr/bin/env python3
"""
Build and flash the Coliseum Combat Arena LED sketches to an ESP32.

Usage:
    python3 flash.py [master|slave] [options]

Examples:
    python3 flash.py master                 # build + flash the master board
    python3 flash.py slave                  # build + flash the slave board
    python3 flash.py slave --build-only     # compile only, don't flash
    python3 flash.py master --port /dev/tty.usbserial-0001
    python3 flash.py slave --monitor

On the first run this automatically downloads PlatformIO (into a local
virtualenv) plus the ESP32 toolchain and sketch libraries - no Arduino IDE
required. Runs on macOS, Linux and Windows.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VENV = ROOT / ".pio-venv"
TARGETS = ("master", "slave")

# The pioarduino ESP32 platform requires Python 3.10+.
PY_MIN = (3, 10)


def info(msg: str) -> None:
    print(f"[combat-led] {msg}")


def step(msg: str) -> None:
    print(f"\n>>> {msg}")


def venv_python() -> Path:
    if os.name == "nt":
        return VENV / "Scripts" / "python.exe"
    return VENV / "bin" / "python"


def run(cmd: list[str]) -> None:
    info("$ " + " ".join(cmd))
    subprocess.run(cmd, cwd=ROOT, check=True)


def python_version(python: str) -> tuple[int, int] | None:
    try:
        out = subprocess.run(
            [python, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if out.returncode == 0:
            major, minor = out.stdout.strip().split(".")
            return int(major), int(minor)
    except Exception:
        pass
    return None


def find_python() -> str:
    ver = python_version(sys.executable)
    if ver and ver >= PY_MIN:
        return sys.executable
    names = ["python3.14", "python3.13", "python3.12", "python3.11", "python3.10",
             "python3", "python"]
    seen = set()
    for name in names:
        path = shutil.which(name)
        if not path or path in seen:
            continue
        seen.add(path)
        ver = python_version(path)
        if ver and ver >= PY_MIN:
            return path
    return ""


def ensure_environment() -> Path:
    base_python = find_python()
    if not base_python:
        sys.exit(
            "\nPython 3.10 or newer is required to build these sketches.\n"
            "Install it from https://www.python.org/downloads/ (or `brew "
            "install python@3.12`) and run this script again.\n"
        )

    py = venv_python()
    if py.exists():
        # Rebuild the venv if it was created with a too-old interpreter.
        venv_py = str(py) + (".exe" if os.name == "nt" else "")
        venv_ver = python_version(venv_py)
        if venv_ver is None or venv_ver < PY_MIN:
            info("Rebuilding build environment with a newer Python...")
            shutil.rmtree(VENV)
            py = venv_python()

    if not py.exists():
        step("Setting up build environment (this is a one-time download)...")
        run([base_python, "-m", "venv", str(VENV)])
        run([str(py), "-m", "pip", "install", "--quiet", "--upgrade", "pip"])
    try:
        subprocess.run(
            [str(py), "-c", "import platformio"],
            check=True,
            capture_output=True,
        )
    except subprocess.CalledProcessError:
        step("Installing PlatformIO...")
        run([str(py), "-m", "pip", "install", "--quiet", "platformio"])
    return py


def main() -> None:
    ap = argparse.ArgumentParser(
        prog=Path(sys.argv[0]).name,
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "target",
        nargs="?",
        default="master",
        choices=TARGETS,
        help=f"which board to build/flash (default: {TARGETS[0]})",
    )
    ap.add_argument(
        "-p",
        "--port",
        help=(
            "USB serial port of the board (e.g. /dev/tty.usbserial-0001, "
            "/dev/ttyUSB0, COM3). Omit to auto-detect."
        ),
    )
    ap.add_argument(
        "--build-only",
        action="store_true",
        help="compile the sketch but do not flash it",
    )
    ap.add_argument(
        "--monitor",
        action="store_true",
        help="open the serial monitor after flashing (or after building)",
    )
    ap.add_argument(
        "--clean",
        action="store_true",
        help="wipe the build cache before compiling",
    )
    args = ap.parse_args()

    py = ensure_environment()
    env = args.target
    project_dir = ROOT / f"led_{env}_esp32"

    step(f"Building '{env}' ({project_dir.name})...")
    if args.clean:
        run([str(py), "-m", "platformio", "run", "-d", str(project_dir), "-t", "clean"])

    build = [str(py), "-m", "platformio", "run", "-d", str(project_dir)]
    if not args.build_only:
        build += ["-t", "upload"]
        if args.port:
            build += ["--upload-port", args.port]
    run(build)

    if args.monitor:
        step(f"Opening serial monitor for '{env}'...")
        mon = [str(py), "-m", "platformio", "device", "monitor", "-d", str(project_dir)]
        if args.port:
            mon += ["--port", args.port]
        subprocess.run(mon, cwd=ROOT)

    if args.build_only:
        info("Build finished. Binary is in %s/.pio/build/%s/firmware.bin"
             % (project_dir.name, env))
    else:
        info(f"Flash finished. If the board did not reset, press its EN/RST button.")


if __name__ == "__main__":
    main()
