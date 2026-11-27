#!/usr/bin/env python3
"""Verify the interior's tamper-evident event log on the PC.

    ~/.platformio/penv/bin/python tools/verify_log.py            # reads the log from the board
    python3 tools/verify_log.py --file dump.txt                 # or from a saved 'log dump'

Checks, with the key derived from include/secrets.h (same as the firmware):
  * every record's HMAC chains from the previous one (no change, insert, delete)
  * sequence numbers are consecutive from the anchor
  * the last record matches the head the board stored in NVS (no truncation)
  * timestamps never go backwards, except at a logged TIME_SET
and lists clock and log warnings the board itself recorded.
Exit code 0 = intact, 1 = tampering or corruption found.
"""
import argparse
import hashlib
import hmac
import os
import re
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
EVENTS = ["?", "BOOT", "GRANTED", "DENIED_PIN", "DENIED_CARD", "DENIED_NO_PIN", "LOCKOUT", "LOCK_REQUEST",
          "LOCK_DENIED_CARD", "LOCKED", "UNLOCKED", "DOOR_OPEN", "DOOR_CLOSED", "ALARM", "ALARM_CLEARED",
          "FAULT", "CARD_ADDED", "CARD_REMOVED", "PIN_CHANGED", "WIPE", "TIME_SET", "CLOCK_JUMP",
          "CLOCK_BACKWARDS", "TIME_INVALID", "LOG_ROLLBACK", "LOG_REPAIRED", "LOCKOUT_RESET", "LATCH_RELEASED",
          "WEB_LOGIN", "WEB_LOGIN_FAIL", "PHONE_PIN", "WEB_ACTION",
          "OTA_START", "OTA_INSTALLED", "OTA_FAIL", "OTA_CONFIRMED", "OTA_ROLLED_BACK"]
WARN_EVENTS = {"OTA_FAIL", "OTA_ROLLED_BACK", "OTA_INSTALLED", "TIME_SET", "CLOCK_JUMP", "CLOCK_BACKWARDS", "TIME_INVALID", "LOG_ROLLBACK", "LOG_REPAIRED", "WIPE"}
RTC_OK, RTC_LOST = 1, 2
TOLERANCE = 5


def psk():
    text = open(os.path.join(HERE, "..", "include", "secrets.h")).read()
    m = re.search(r"ATDS_PSK\[32\]\s*=\s*\{([^}]*)\}", text)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def read_board():
    sys.path.insert(0, HERE)
    import serial
    port = subprocess.run([sys.executable, os.path.join(HERE, "find_port.py"), "interior"],
                          capture_output=True, text=True).stdout.strip()
    if not port:
        sys.exit("interior board not found (close any serial monitor)")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.3
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.5)
    s.reset_input_buffer()
    s.write(b"log dump\n")
    buf, end = b"", time.time() + 120
    while b"ATDSLOG end" not in buf and time.time() < end:
        buf += s.read(65536)
    s.close()
    if b"ATDSLOG end" not in buf:
        sys.exit("no complete dump received (is env:interior / interior_bench / interior_sim flashed?)")
    return buf.decode(errors="replace")


def fmt_time(t, state):
    if state not in (RTC_OK, RTC_LOST) or not t:
        return "no clock".ljust(20)
    s = datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    return s + ("?" if state == RTC_LOST else " ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", help="saved output of the console command 'log dump'")
    ap.add_argument("--all", action="store_true", help="print every record, not just warnings")
    args = ap.parse_args()
    text = open(args.file).read() if args.file else read_board()

    m = re.search(r"ATDSLOG 1 anchor (\d+) ([0-9a-f]{32}) head (\d+) ([0-9a-f]{32})", text)
    if not m:
        sys.exit("no ATDSLOG header found")
    anchor_seq, prev = int(m[1]), bytes.fromhex(m[2])
    head_seq, head_tag = int(m[3]), bytes.fromhex(m[4])
    recs = [bytes.fromhex(h) for h in re.findall(r"^R ([0-9a-f]{96})\s*$", text, re.M)]

    key = hmac.new(psk(), b"ATDS log key v1", hashlib.sha256).digest()
    problems, warnings, last_time = [], [], 0
    expect = anchor_seq
    for raw in recs:
        body, tag = raw[:32], raw[32:]
        seq, boot, up, t, st, ev, slot, detail, a1, a2 = struct.unpack("<IIIIBBBBII4x", body)
        name = EVENTS[ev] if ev < len(EVENTS) else f"?{ev}"
        good = hmac.compare_digest(hmac.new(key, prev + body, hashlib.sha256).digest()[:16], tag)
        line = f"#{seq:<6} {fmt_time(t, st)} boot {boot} +{up // 1000}s  {name}"
        if slot != 0xFF:
            line += f" card {slot}"
        if detail:
            line += f" d={detail}"
        if a1 or a2:
            line += f" {a1} {a2}"
        if seq != expect:
            problems.append(f"sequence jumps from {expect} to {seq}: records missing or inserted")
        if not good:
            problems.append(f"#{seq}: HMAC does not match: record changed, or chain broken before it")
        if st == RTC_OK:
            if t + TOLERANCE < last_time and name != "TIME_SET":
                warnings.append(f"#{seq}: time goes backwards ({fmt_time(t, st).strip()})")
            last_time = t
        if name in WARN_EVENTS:
            warnings.append(line)
        if args.all:
            print(("   " if good else "!! ") + line)
        prev, expect = tag, seq + 1
        if problems and not args.all:
            break
    if not problems and (expect - 1 != head_seq or prev != head_tag):
        problems.append(f"log ends at #{expect - 1} but the board's stored head is #{head_seq}: "
                        "records removed from the end, or the log was rolled back")

    print(f"\n{len(recs)} record(s), anchor #{anchor_seq}, head #{head_seq}")
    for w in warnings:
        print(f"WARN  {w}")
    for p in problems:
        print(f"FAIL  {p}")
    print("RESULT: " + ("LOG INTACT" if not problems else "TAMPERING OR CORRUPTION FOUND"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
