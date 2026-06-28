#!/usr/bin/env python3
"""
flash_watch.py — batch-flash dualETH-PixelControl boards as they're plugged in.

Usage:
    python3 tools/flash_watch.py            # watch and flash any new board
    python3 tools/flash_watch.py --include-existing   # also flash boards already plugged in at start

On startup the script snapshots currently-connected serial ports and ignores
them (assume they're dev tools / unrelated). When a new /dev/cu.* device
appears it runs `pio run -e esp07 -t upload --upload-port <port>`. After
the upload completes the port is remembered until it's unplugged again,
so leaving a flashed board connected won't cause a reflash loop.

Ctrl-C to stop.
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


PIO = find_pio()


def list_ports() -> set[str]:
    found: set[str] = set()
    for pattern in PORT_GLOBS:
        found.update(glob.glob(pattern))
    return found


def flash(port: str) -> bool:
    print(f"\n→ flashing {port}", flush=True)
    cmd = [PIO, "run", "-e", "esp07", "-t", "upload", "--upload-port", port]
    result = subprocess.run(cmd, cwd=PROJECT_DIR)
    ok = result.returncode == 0
    print(f"{'✓ done' if ok else '✗ FAILED'} {port}\n", flush=True)
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument(
        "--include-existing",
        action="store_true",
        help="also flash ports that were already plugged in at startup",
    )
    args = parser.parse_args()

    print("Pre-building firmware (cached for subsequent uploads)…", flush=True)
    if subprocess.run([PIO, "run", "-e", "esp07"], cwd=PROJECT_DIR).returncode != 0:
        print("Build failed — fix it before connecting boards.", file=sys.stderr)
        return 1

    seen_at_startup = set() if args.include_existing else list_ports()
    flashed: set[str] = set()

    if seen_at_startup:
        print(f"Ignoring {len(seen_at_startup)} pre-existing port(s): "
              f"{sorted(seen_at_startup)}", flush=True)
    print("Watching for new boards. Plug one in to flash. Ctrl-C to stop.", flush=True)

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
                if flash(port):
                    flashed_count += 1
                else:
                    failed_count += 1
                flashed.add(port)
                print(f"Total: {flashed_count} flashed, {failed_count} failed. "
                      "Unplug and connect the next board.", flush=True)
            time.sleep(POLL_INTERVAL_S)
    except KeyboardInterrupt:
        print(f"\nStopped. {flashed_count} flashed, {failed_count} failed.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
