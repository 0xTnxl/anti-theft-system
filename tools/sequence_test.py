#!/usr/bin/env python3
"""Automated test of the shared-PIN, unlock/hold/release, card+A lock and log flow.

Bare boards are enough (no keypad, reader or motor needed):
    pio run -e interior_sim  -t upload --upload-port $(python3 tools/find_port.py interior)
    pio run -e exterior_test -t upload --upload-port $(python3 tools/find_port.py exterior)
    ~/.platformio/penv/bin/python tools/sequence_test.py [-v]
Afterwards flash env:exterior (and env:interior) again.

It WIPES all cards and the shared PIN on the interior (asks first unless --yes).
With env:interior_bench instead of interior_sim the real motor moves.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from link_test import Board, Fail, port_of  # noqa: E402

CARD, OTHER = "04A1B2C3", "DEADBEEF"
PIN, NEWPIN = "4826", "1357"


def main():
    ap = argparse.ArgumentParser(description="sequence test")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--yes", action="store_true", help="do not ask before wiping cards")
    args = ap.parse_args()
    if not args.yes and input("This deletes every card and the shared PIN on the interior. Continue? [y/n] ") != "y":
        return 1
    intr = Board("INT", port_of("interior"), args.verbose)
    ext = Board("EXT", port_of("exterior"), args.verbose)
    results = []

    def con(cmd, pat, t=3):
        m = intr.mark()
        intr.send(cmd)
        return intr.expect(pat, t, m)

    def ext_cmd(cmd, want):
        end = time.time() + 15
        while True:
            m = ext.mark()
            ext.send(cmd)
            if ext.expect(r"\[test\] (auth sent|lock sent|not ready|bad|usage)", 3, m)[1] in ("auth sent", "lock sent"):
                break
            if time.time() > end:
                raise Fail("exterior never ready")
            time.sleep(0.5)
        r = ext.expect(r"\[auth\] result (\w+) attempts_left=(\d+)", 8, m)
        if r[1] != want:
            raise Fail(f"'{cmd}': expected {want}, got {r[1]}")
        time.sleep(1.2)
        return r

    def step(name, fn):
        print(f"  ... {name}", flush=True)
        try:
            results.append((name, True, fn()))
        except Fail as e:
            results.append((name, False, str(e)))
            raise

    def s_boot():
        mi, me = intr.mark(), ext.mark()
        intr.reset()
        ext.reset()
        intr.expect(r"\[int\] anti-theft interior", 10, mi)
        bench = bool(intr.find(r"BENCH BUILD", mi)) or bool(intr.expect(r"SIMULATED BOLT|BENCH BUILD", 5, mi)[0].find("BENCH") >= 0)
        intr.expect(r"\[link\] session established", 20, mi)
        ext.expect(r"TEST BUILD", 5, me)
        ext.expect(r"\[link\] session confirmed", 20, me)
        return f"link up ({'REAL motor' if bench else 'simulated bolt'})"

    def s_setup():
        con("wipe yes", r"all cards deleted")
        con("resetlockout", r"lockout reset")
        con("door close", r"\[sim\] door")
        st = con("status", r"sharedPIN=(\S+)")
        if st[1] != "NOT":
            raise Fail("shared PIN still set after wipe")
        con("enroll", r"no shared PIN yet")
        ext_cmd(f"auth {CARD} {PIN}", "ENROLL_AGAIN")
        m = intr.mark()
        ext_cmd(f"auth {CARD} {PIN}", "ENROLL_OK")
        intr.expect(r"shared PIN set; stored in slot (\d+)", 3, m)
        con("enroll", r"enter the shared PIN")
        ext_cmd(f"auth {OTHER} 9999", "ENROLL_FAIL")
        con("enroll", r"enter the shared PIN")
        ext_cmd(f"auth {OTHER} {PIN}", "ENROLL_OK")
        n = con("list", r"(\d+) card\(s\), shared PIN set")
        return f"first enroll set the PIN; second card needed the shared PIN (wrong one refused); {n[1]} cards"

    def s_unlock_hold_release():
        m = intr.mark()
        ext_cmd(f"auth {OTHER} {PIN}", "GRANTED")
        intr.expect(r"LOCKED -> UNLOCKING", 3, m)
        intr.expect(r"UNLOCKING -> HOLDING", 10, m)
        mv = intr.find(r"\] (turns done|end switch reached) in (\d+) ms.*position ([\d.]+) turns", m)
        time.sleep(1)
        con("door open", r"door OPEN")
        intr.expect(r"releasing the latch \(door opened\)", 4, m)
        intr.expect(r"RELEASING -> UNLOCKED", 6, m)
        pos = con("status", r"position=([-\d.]+) turns")
        return f"second card + shared PIN unlocked to {mv[-1].split('position ')[1] if mv else '?'}, held, released at door open, now at {pos[1]} turns"

    def s_lock():
        r = ext_cmd(f"lock {CARD}", "LOCK_REFUSED")
        if r[2] != "1":
            raise Fail("refusal reason was not 'door open'")
        con("door close", r"door closed")
        m = intr.mark()
        ext_cmd(f"lock {CARD}", "LOCKING")
        intr.expect(r"UNLOCKED -> LOCKING", 3, m)
        intr.expect(r"LOCKING -> LOCKED", 8, m)
        pos = con("status", r"position=([-\d.]+) turns")
        ext_cmd(f"lock {CARD}", "LOCKING")   # already locked: accepted, no move
        return f"refused with door open; card + A locked back to {pos[1]} turns; repeat while locked is a no-op"

    def s_lock_unknown():
        m = intr.mark()
        ext_cmd("lock 11223344", "DENIED")
        intr.expect(r"request DENIED \(unknown card\)", 3, m)
        return "unknown card cannot lock, counted as a failure"

    def s_hold_timeout():
        m = intr.mark()
        ext_cmd(f"auth {CARD} {PIN}", "GRANTED")
        intr.expect(r"UNLOCKING -> HOLDING", 10, m)
        intr.expect(r"releasing the latch \(hold timed out\)", 20, m)
        intr.expect(r"RELEASING -> UNLOCKED", 6, m)
        ext_cmd(f"lock {CARD}", "LOCKING")
        intr.expect(r"LOCKING -> LOCKED", 8, m)
        return "door never opened: hold released after the time limit; locked again"

    def s_setpin():
        con("setpin", r"\[setpin\] tap an enrolled card")
        ext_cmd(f"auth {CARD} {NEWPIN}", "ENROLL_AGAIN")
        ext_cmd(f"auth {CARD} {NEWPIN}", "PIN_SET")
        ext_cmd(f"auth {OTHER} {PIN}", "DENIED")
        m = intr.mark()
        ext_cmd(f"auth {OTHER} {NEWPIN}", "GRANTED")
        intr.expect(r"UNLOCKING -> HOLDING", 10, m)
        con("door open", r"door OPEN")
        intr.expect(r"RELEASING -> UNLOCKED", 8, m)
        con("door close", r"door closed")
        ext_cmd(f"lock {CARD}", "LOCKING")
        intr.expect(r"LOCKING -> LOCKED", 8, m)
        con("resetlockout", r"lockout reset")
        return "PIN changed with an enrolled card; old PIN refused for both cards; new PIN works"

    def s_forced():
        m = intr.mark()
        con("door open", r"door OPEN")
        intr.expect(r"\[alarm\] RAISED: FORCED_DOOR", 3, m)
        con("door close", r"door closed")
        con("clear", r"cleared by console")
        return "door opened while locked raised FORCED_DOOR"

    def s_log():
        v = con("log verify", r"\[log\] (chain OK: (\d+)|CHAIN BROKEN)", 10)
        if not v[1].startswith("chain OK"):
            raise Fail(v[0])
        intr.send("log show 400")
        time.sleep(2)
        for ev in ("CARD_ADDED", "PIN_CHANGED", "GRANTED", "UNLOCKED", "LATCH_RELEASED", "LOCK_REQUEST",
                   "LOCKED", "LOCK_DENIED_CARD", "DOOR_OPEN", "ALARM", "DENIED_PIN"):
            if not intr.find(rf"\s{ev}\b"):
                raise Fail(f"log has no {ev} record")
        return f"on-board check: chain intact over {v[2]} records; every tested event is in the log"

    try:
        for name, fn in [("boot", s_boot), ("shared PIN + enroll", s_setup),
                         ("unlock, hold, release", s_unlock_hold_release), ("card + A lock", s_lock),
                         ("lock with unknown card", s_lock_unknown), ("hold timeout", s_hold_timeout),
                         ("change shared PIN", s_setpin), ("forced door", s_forced), ("event log", s_log)]:
            step(name, fn)
    except Fail:
        pass
    print("\n================ SEQUENCE TEST SUMMARY ================")
    for n, ok, note in results:
        print(f"{'PASS' if ok else 'FAIL'}  {n:24} {note}")
    intr.close()
    ext.close()
    return 0 if all(ok for _, ok, _ in results) and len(results) == 9 else 1


if __name__ == "__main__":
    sys.exit(main())
