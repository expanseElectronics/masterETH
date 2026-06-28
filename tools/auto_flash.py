#!/usr/bin/env python3
"""
auto_flash.py — erase and flash masterETH boards.

Usage:
    python3 tools/auto_flash.py /dev/cu.usbserial-2120    # erase + flash specific port
    python3 tools/auto_flash.py --watch                   # watch for new boards and erase + flash each
    python3 tools/auto_flash.py --watch --include-existing   # also flash existing ports

The tool ALWAYS erases the full flash before uploading firmware — this ensures
a clean slate for each board and avoids stale EEPROM/LittleFS data from previous
firmware versions.

For production batch flashing, use --watch mode. Boards will be erased and flashed
as they're plugged in. After flashing, keep the board connected until the status
LED shows the boot indicator (dim cyan), then unplug and connect the next board.

Ctrl-C to stop in watch mode.
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

PORT_GLOBS = [
    "/dev/cu.usbserial*",
    "/dev/cu.usbmodem*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.SLAB_USBtoUART*",
]
POLL_INTERVAL_S = 0.5
PROJECT_DIR = Path(__file__).resolve().parent.parent


def find_pio() -> str:
    """Locate the pio executable, falling back to the standard PlatformIO penv."""
    found = shutil.which("pio")
    if found:
        return found
    fallback = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if fallback.is_file() and os.access(fallback, os.X_OK):
        return str(fallback)
    print("Could not find the 'pio' executable. Install PlatformIO Core or add "
          "it to PATH (e.g. ~/.platformio/penv/bin).", file=sys.stderr)
    sys.exit(1)


def find_esptool() -> str:
    """Locate esptool.py in the PlatformIO packages directory."""
    pio_packages = Path.home() / ".platformio" / "packages"
    candidates = list(pio_packages.glob("tool-esptoolpy*/esptool.py"))
    if candidates:
        return str(candidates[0])
    print("Could not find esptool.py in ~/.platformio/packages/", file=sys.stderr)
    print("Run 'pio run' once to install the toolchain.", file=sys.stderr)
    sys.exit(1)


PIO = find_pio()
ESPTOOL = find_esptool()


def list_ports() -> set[str]:
    found: set[str] = set()
    for pattern in PORT_GLOBS:
        found.update(glob.glob(pattern))
    return found


def erase_flash(port: str) -> bool:
    """Erase the entire flash on the given port using esptool.py."""
    print(f"  [1/2] Erasing flash on {port}...", flush=True)
    cmd = ["python3", ESPTOOL, "--port", port, "erase_flash"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        print(f"  ✓ Erase complete", flush=True)
        return True
    else:
        print(f"  ✗ Erase FAILED", flush=True)
        if "ESP32" in result.stdout:
            print(f"  Error: Wrong chip detected (ESP32 instead of ESP8266).", file=sys.stderr)
            print(f"  Make sure the masterETH board (not another ESP32 project) is connected.", file=sys.stderr)
        elif result.stderr:
            print(result.stderr, file=sys.stderr)
        return False


def flash_firmware(port: str) -> bool:
    """Flash the firmware to the given port using PlatformIO."""
    print(f"  [2/2] Flashing firmware to {port}...", flush=True)
    cmd = [PIO, "run", "-e", "esp07", "-t", "upload", "--upload-port", port]
    result = subprocess.run(cmd, cwd=PROJECT_DIR, capture_output=True, text=True)
    if result.returncode == 0:
        print(f"  ✓ Flash complete", flush=True)
        return True
    else:
        print(f"  ✗ Flash FAILED", flush=True)
        if "ESP32" in result.stdout or "ESP32" in result.stderr:
            print(f"  Error: Wrong chip detected (ESP32 instead of ESP8266).", file=sys.stderr)
            print(f"  Make sure the masterETH board is connected.", file=sys.stderr)
        elif result.stderr:
            # Only print first 10 lines of error to keep output clean
            lines = result.stderr.strip().split('\n')
            for line in lines[-10:]:
                print(f"    {line}", file=sys.stderr)
        return False


def erase_and_flash(port: str) -> bool:
    """Erase and flash a board. Returns True if both steps succeed."""
    print(f"\n{'='*60}")
    print(f"→ Processing {port}")
    print(f"{'='*60}", flush=True)

    if not erase_flash(port):
        print(f"✗ FAILED: {port} (erase step failed)\n", flush=True)
        return False

    # Small delay to let the board reset after erase
    time.sleep(0.5)

    if not flash_firmware(port):
        print(f"✗ FAILED: {port} (flash step failed)\n", flush=True)
        return False

    print(f"\n✓ SUCCESS: {port}")
    print(f"   Wait for boot indicator (dim cyan status LED),")
    print(f"   then unplug and connect the next board.\n", flush=True)
    return True


def flash_single_port(port: str) -> int:
    """Flash a single port and exit."""
    if not os.path.exists(port):
        print(f"Error: Port {port} not found.", file=sys.stderr)
        available = sorted(list_ports())
        if available:
            print(f"Available ports: {available}", file=sys.stderr)
        else:
            print(f"No serial ports detected.", file=sys.stderr)
        return 1

    print("Pre-building firmware…", flush=True)
    result = subprocess.run([PIO, "run", "-e", "esp07"], cwd=PROJECT_DIR, capture_output=True)
    if result.returncode != 0:
        print("Build failed.", file=sys.stderr)
        if result.stderr:
            print(result.stderr.decode('utf-8'), file=sys.stderr)
        return 1

    return 0 if erase_and_flash(port) else 1


def watch_and_flash(include_existing: bool) -> int:
    """Watch for new boards and erase + flash each one."""
    print("Pre-building firmware (cached for subsequent uploads)…", flush=True)
    result = subprocess.run([PIO, "run", "-e", "esp07"], cwd=PROJECT_DIR, capture_output=True)
    if result.returncode != 0:
        print("Build failed — fix it before connecting boards.", file=sys.stderr)
        if result.stderr:
            print(result.stderr.decode('utf-8'), file=sys.stderr)
        return 1

    seen_at_startup = set() if include_existing else list_ports()
    flashed: set[str] = set()

    if seen_at_startup:
        print(f"Ignoring {len(seen_at_startup)} pre-existing port(s): "
              f"{sorted(seen_at_startup)}", flush=True)
    print("\nWatching for new boards. Plug one in to erase + flash. Ctrl-C to stop.\n", flush=True)

    flashed_count = 0
    failed_count = 0
    try:
        while True:
            current = list_ports()
            # Forget ports that were unplugged so reinsertion triggers a reflash.
            flashed &= current
            seen_at_startup &= current

            new_ports = current - seen_at_startup - flashed
            for port in sorted(new_ports):
                if erase_and_flash(port):
                    flashed_count += 1
                else:
                    failed_count += 1
                flashed.add(port)
                print(f"{'─'*60}")
                print(f"Total: {flashed_count} flashed, {failed_count} failed.")
                print(f"{'─'*60}\n", flush=True)
            time.sleep(POLL_INTERVAL_S)
    except KeyboardInterrupt:
        print(f"\n{'='*60}")
        print(f"Stopped. {flashed_count} flashed, {failed_count} failed.")
        print(f"{'='*60}")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Erase and flash masterETH boards",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage:")[1] if "Usage:" in __doc__ else None,
    )
    parser.add_argument(
        "port",
        nargs="?",
        help="Serial port to flash (e.g. /dev/cu.usbserial-2120). Omit to use --watch mode.",
    )
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Watch for new boards and erase + flash each one as it's plugged in",
    )
    parser.add_argument(
        "--include-existing",
        action="store_true",
        help="In --watch mode, also flash ports that were already plugged in at startup",
    )
    args = parser.parse_args()

    if args.port and args.watch:
        print("Error: Cannot specify both a port and --watch mode.", file=sys.stderr)
        return 1

    if args.include_existing and not args.watch:
        print("Error: --include-existing requires --watch mode.", file=sys.stderr)
        return 1

    if args.port:
        return flash_single_port(args.port)
    elif args.watch:
        return watch_and_flash(args.include_existing)
    else:
        parser.print_help()
        print("\nError: Specify either a port or --watch mode.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
