#!/usr/bin/env python3
"""Print the serial port of the board with a given factory MAC or role.

Linux numbers USB serial ports in detection order, and both boards' USB chips
report the same serial number, so the factory MAC is the only reliable way to
tell them apart.

    python3 tools/find_port.py interior            # role from tools/boards.json
    python3 tools/find_port.py 70:4b:ca:04:97:20   # or a MAC directly
    python3 tools/find_port.py --list              # every port and its MAC

    pio run -e interior -t upload --upload-port $(python3 tools/find_port.py interior)

Reading the MAC briefly resets the board into its bootloader. Close any serial
monitor first, or that port shows as busy.
"""
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PIO = os.path.expanduser("~/.platformio")
ESPTOOL = os.path.join(PIO, "packages", "tool-esptoolpy", "esptool.py")
PIO_PY = os.path.join(PIO, "penv", "bin", "python")
MAC_RE = re.compile(r"^([0-9a-f]{2}:){5}[0-9a-f]{2}$")


def norm(mac: str) -> str:
    return mac.strip().lower().replace("-", ":")


def ports():
    return sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))


def read_mac(port: str):
    """Returns (mac, None) or (None, error text)."""
    py = PIO_PY if os.path.exists(PIO_PY) else sys.executable
    try:
        out = subprocess.run([py, ESPTOOL, "--port", port, "read_mac"], capture_output=True,
                             text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None, "timed out"
    m = re.search(r"MAC:\s*(([0-9a-f]{2}:){5}[0-9a-f]{2})", out.stdout, re.I)
    if m:
        return norm(m.group(1)), None
    text = (out.stdout + out.stderr).strip().splitlines()
    err = next((l for l in reversed(text) if "error" in l.lower() or "busy" in l.lower()),
               text[-1] if text else "no output")
    return None, err


def resolve(target: str) -> str:
    t = norm(target)
    if MAC_RE.match(t):
        return t
    with open(os.path.join(HERE, "boards.json")) as f:
        boards = json.load(f)
    if target not in boards or target.startswith("_"):
        roles = ", ".join(k for k in boards if not k.startswith("_"))
        sys.exit(f"unknown role '{target}' (tools/boards.json has: {roles})")
    return norm(boards[target])


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    if not os.path.exists(ESPTOOL):
        sys.exit(f"esptool not found at {ESPTOOL}")
    found = ports()
    if not found:
        sys.exit("no /dev/ttyUSB* or /dev/ttyACM* ports")

    if sys.argv[1] == "--list":
        for p in found:
            mac, err = read_mac(p)
            print(f"{p}  {mac or 'ERROR: ' + err}")
        return 0

    want = resolve(sys.argv[1])
    errors = []
    for p in found:
        mac, err = read_mac(p)
        if mac == want:
            print(p)
            return 0
        if err:
            errors.append(f"  {p}: {err}")
    msg = f"no board with MAC {want} found"
    if errors:
        msg += "\nports that could not be read:\n" + "\n".join(errors)
    print(msg, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
