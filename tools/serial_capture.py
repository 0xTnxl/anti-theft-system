#!/usr/bin/env python3
"""Reset one or more boards and print their serial output for a fixed time.

    ~/.platformio/penv/bin/python tools/serial_capture.py 20 interior exterior
    ~/.platformio/penv/bin/python tools/serial_capture.py 10 /dev/ttyUSB0

Arguments after the duration are roles from tools/boards.json, MACs, or ports.
Lines are printed per board, prefixed with its name.
"""
import os
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python")

HERE = os.path.dirname(os.path.abspath(__file__))


def to_port(target):
    if target.startswith("/dev/"):
        return target
    r = subprocess.run([sys.executable, os.path.join(HERE, "find_port.py"), target],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"cannot find {target}: {r.stderr.strip()}")
    return r.stdout.strip()


def capture(name, port, secs, out):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    s.rts = True            # pulse EN: reset into the application
    time.sleep(0.1)
    s.rts = False
    buf, t0 = bytearray(), time.time()
    while time.time() - t0 < secs:
        buf.extend(s.read(512))
    s.close()
    text = bytes(buf).decode(errors="replace").replace("\r", "")
    out.extend(f"{name[:8]:8} | {l}" for l in text.split("\n") if l.strip())


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    secs = float(sys.argv[1])
    jobs = [(t, to_port(t)) for t in sys.argv[2:]]
    outs = {name: [] for name, _ in jobs}
    threads = [threading.Thread(target=capture, args=(n, p, secs, outs[n])) for n, p in jobs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for name in outs:
        print("\n".join(outs[name]))
        print("-" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
