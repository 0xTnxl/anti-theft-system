#!/usr/bin/env python3
"""Make a board blink so you can tell the two DevKits apart.

    ~/.platformio/penv/bin/python tools/identify.py interior      # fast blink
    ~/.platformio/penv/bin/python tools/identify.py exterior 60   # slow blink

Interior: onboard blue LED (and red LED on GPIO4) blink FAST, 5 times a second.
Exterior: onboard blue LED (and red LED on GPIO16) blink SLOW, once a second.

Needs the production firmware (env:interior / env:exterior). Opening the serial
port resets the board, so this waits for it to boot before sending "identify".
"""
import os
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python")

HERE = os.path.dirname(os.path.abspath(__file__))
BANNER = {"interior": b"[int] anti-theft", "exterior": b"[ext] anti-theft"}


def main():
    if len(sys.argv) not in (2, 3) or sys.argv[1] not in BANNER:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    role = sys.argv[1]
    secs = int(sys.argv[2]) if len(sys.argv) == 3 else 30
    r = subprocess.run([sys.executable, os.path.join(HERE, "find_port.py"), role],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"cannot find the {role} board: {r.stderr.strip()}")
    port = r.stdout.strip()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    s.rts = True            # reset on purpose, so the boot banner is fresh
    time.sleep(0.1)
    s.rts = False
    buf, end = b"", time.time() + 10
    while BANNER[role] not in buf and time.time() < end:
        buf += s.read(512)
    if BANNER[role] not in buf:
        sys.exit(f"{port}: no {role} firmware banner. Is env:{role} flashed on this board?")
    time.sleep(1.0)         # let setup() finish
    s.reset_input_buffer()
    s.write(f"identify {secs}\n".encode())
    reply, end = b"", time.time() + 3
    while b"board:" not in reply and time.time() < end:
        reply += s.read(256)
    s.close()               # closing does not reset the board, the blink continues
    line = next((l for l in reply.decode(errors="replace").splitlines() if "board:" in l), None)
    if not line:
        sys.exit(f"{port}: board did not confirm the identify command")
    print(f"{port}: {line.strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
