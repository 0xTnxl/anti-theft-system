#!/usr/bin/env python3
"""Guided end-to-end test of the exterior user flow with real components:
RC522 card -> keypad PIN -> OLED / LEDs -> secure link -> interior decision.
The bolt is simulated (env:interior_sim), so no motor or limit switches are needed.

Flash first (ports picked by board MAC):
    pio run -e interior_sim -t upload --upload-port $(python3 tools/find_port.py interior)
    pio run -e exterior     -t upload --upload-port $(python3 tools/find_port.py exterior)
Run with PlatformIO's Python, operator at the PC, bench person at the exterior:
    ~/.platformio/penv/bin/python tools/flow_test.py [--skip lockout,unknown]

The script says what to do, watches both serial logs, and asks y/n for what only
a person can see or hear. Each step waits up to 60 s for the bench person.
It enrolls one card and deletes it again at the end (other cards are kept).
Both boards' logs are saved to flow_test_<date>.log. About 15 minutes.
Afterwards flash env:interior again: never fit env:interior_sim to a door.

See docs/FLOW_TEST.md.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from link_test import Board, Fail, port_of  # noqa: E402

WAIT = 60   # seconds a person gets for each action

# Exterior STATUS values (lib/protocol/protocol.h)
LOCKED, UNLOCKED = 1, 3


def main():
    ap = argparse.ArgumentParser(description="guided exterior -> interior flow test")
    ap.add_argument("--skip", default="", help="comma list of optional steps: unknown,lockout,linkloss")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo every serial line")
    args = ap.parse_args()
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}

    pi, pe = port_of("interior"), port_of("exterior")
    print(f"interior {pi}, exterior {pe}\n")
    intr, ext = Board("INT", pi, args.verbose), Board("EXT", pe, args.verbose)
    state = {"slot": None}

    def check(cond, msg):
        if not cond:
            raise Fail(msg)

    def say(text):
        print(f"\n  >>> {text}", flush=True)

    def confirm(question):
        while True:
            a = input(f"  ??? {question} [y/n] ").strip().lower()
            if a in ("y", "n"):
                check(a == "y", f"operator answered no: {question}")
                return
            if not a:
                continue

    def cmd(text, pattern, timeout=3):
        m = intr.mark()
        intr.send(text)
        return intr.expect(pattern, timeout, m)

    def card_and_pin(what, want, attempts=None):
        """Bench person taps a card and types a PIN. Returns the AUTH_RESULT match."""
        me, mi = ext.mark(), intr.mark()
        say(what)
        ext.expect(r"\[rfid\] card read, (\d+)-byte UID", WAIT, me)
        ext.expect(r"\[ui\] CardWait -> PinEntry", 3, me)
        r = ext.expect(r"\[auth\] result (\w+) attempts_left=(\d+) lockout=(\d+)s", WAIT, me)
        digits = len(ext.find(r"\[key\] digit", me))
        check(r[1] == want, f"expected {want}, got {r[1]}")
        if attempts is not None:
            check(int(r[2]) == attempts, f"expected {attempts} attempts left, got {r[2]}")
        return r, me, mi, digits

    def wait_cardwait(since, timeout=10):
        ext.expect(r"-> CardWait", timeout, since)

    # ------------------------------------------------------------- steps ---

    def s_boot():
        mi, me = intr.mark(), ext.mark()
        intr.reset()
        ext.reset()
        ext.expect(r"\[ext\] anti-theft exterior module", 10, me)
        try:
            intr.expect(r"SIMULATED BOLT", 10, mi)
        except Fail:
            raise Fail("interior is not running env:interior_sim (no SIMULATED BOLT banner). "
                       "The real interior build would drive the motor: flash interior_sim first")
        intr.expect(r"\[link\] session established", 20, mi)
        ext.expect(r"\[link\] session confirmed", 20, me)
        ext.expect(r"\[ui\] Linking -> CardWait", 10, me)
        oled = ext.find(r"SSD1306 not found", me)
        check(not oled, "exterior reports the OLED missing (SSD1306 not found at 0x3C)")
        ext.expect(r"\[rfid\] RC522 ready", 10, me)
        return "link up, exterior on the tap-card screen, OLED and RC522 found"

    def s_prep():
        st = cmd("status", r"cards=(\d+)/\d+ fails=(\d+)")
        cmd("resetlockout", r"lockout reset")
        return f"interior had {st[1]} card(s); failure counter reset"

    def s_idle_screen():
        confirm("OLED shows 'LOCKED' on top and 'TAP CARD' in large text, and all 3 exterior LEDs are off?")
        return "idle screen and LEDs confirmed"

    def s_enroll():
        cmd("enroll", r"\[enroll\] tap")
        time.sleep(0.5)
        confirm("OLED top line now reads 'ENROLL MODE' and the interior red LED is on (if wired)?")
        print("  Choose a test PIN of 4-8 digits and tell the bench person. Use it for every step.")
        card_and_pin("Tap the TEST CARD, type the test PIN, press #", "ENROLL_AGAIN")
        confirm("OLED showed 'SAME CARD + PIN / REPEAT' with the yellow LED?")
        _, _, mi, _ = card_and_pin("Tap the SAME card again, type the SAME PIN, press #", "ENROLL_OK")
        g = intr.expect(r"stored in slot (\d+) \(PBKDF2 took (\d+) ms\)", 3, mi)
        state["slot"] = int(g[1])
        confirm("OLED showed 'CARD ENROLLED / SAVED' with the green LED?")
        return f"enrolled in slot {g[1]} (PIN hash {g[2]} ms)"

    def s_grant():
        r, me, mi, digits = card_and_pin("Tap the test card, type the test PIN, press #", "GRANTED")
        intr.expect(r"\[auth\] GRANTED", 3, mi)
        intr.expect(r"\(simulated\) bolt retracted", 5, mi)
        ext.expect(rf"\[status\] lock={UNLOCKED} ", 8, me)
        confirm("OLED showed 'ACCESS GRANTED / WELCOME' with the GREEN LED, then 'UNLOCKED / TAP CARD'?")
        t0, me2 = time.time(), ext.mark()
        intr.expect(r"\(simulated\) bolt thrown", 20, mi)
        ext.expect(rf"\[status\] lock={LOCKED} ", 5, me2)
        return f"GRANTED; interior unlocked; auto-relocked after {time.time() - t0 + 1:.0f} s (door never opened)"

    def s_door_cycle():
        r, me, mi, _ = card_and_pin("Tap the test card, type the test PIN, press # (we then 'open' the door)", "GRANTED")
        intr.expect(r"\(simulated\) bolt retracted", 5, mi)
        cmd("door open", r"\[sim\] door OPEN")
        time.sleep(2)
        m = intr.mark()
        cmd("door close", r"\[sim\] door closed")
        t0 = time.time()
        intr.expect(r"\(simulated\) bolt thrown", 10, m)
        alarms = intr.find(r"\[alarm\] RAISED", mi)
        check(not alarms, f"opening the door while unlocked raised an alarm: {alarms[0] if alarms else ''}")
        return f"door opened and closed while unlocked: no alarm, relocked {time.time() - t0:.1f} s after closing"

    def s_cancel():
        me, mi = ext.mark(), intr.mark()
        say("Tap the test card, then press * without typing any digit")
        ext.expect(r"\[rfid\] card read", WAIT, me)
        ext.expect(r"\[key\] \*", WAIT, me)
        ext.expect(r"PinEntry -> CardWait", 3, me)
        check(not intr.find(r"\[auth\]", mi), "interior saw an auth request after a cancel")
        return "'*' on an empty PIN returns to the tap-card screen, nothing sent"

    def s_short_pin():
        me, mi = ext.mark(), intr.mark()
        say("Tap the test card, type only 2 digits, press #")
        ext.expect(r"\[rfid\] card read", WAIT, me)
        ext.expect(r"PIN too short \(2 digits\), not sent", WAIT, me)
        confirm("OLED top line shows 'MIN 4 DIGITS'?")
        say("Now press * twice (first clears the digits, second cancels)")
        ext.expect(r"PinEntry -> CardWait", WAIT, me)
        check(not intr.find(r"\[auth\]", mi), "interior saw an auth request for a 2-digit PIN")
        return "2-digit PIN refused on the exterior, never sent; '*' clears then cancels"

    def s_timeout():
        me = ext.mark()
        say("Tap the test card, then do NOT touch the keypad (15 s)")
        ext.expect(r"\[rfid\] card read", WAIT, me)
        t0 = time.time()
        ext.expect(r"PIN entry timed out", 20, me)
        ext.expect(r"PinEntry -> CardWait", 2, me)
        return f"PIN entry abandoned after {time.time() - t0:.0f} s, back to tap-card"

    def s_wrong_pin():
        _, _, mi, _ = card_and_pin("Tap the test card, type a WRONG PIN (4+ digits), press #", "DENIED", attempts=4)
        intr.expect(r"DENIED \(wrong PIN\)", 3, mi)
        confirm("OLED showed '4 TRIES LEFT / DENIED' with the RED LED, and the interior buzzer gave a low beep (if wired)?")
        return "wrong PIN: DENIED, 4 attempts left"

    def s_unknown():
        _, _, mi, _ = card_and_pin("Tap a DIFFERENT card (not enrolled), type the test PIN, press #", "DENIED", attempts=3)
        intr.expect(r"DENIED \(unknown card\)", 3, mi)
        return "unknown card: DENIED"

    def s_reset_after_denied():
        _, _, mi, _ = card_and_pin("Tap the test card, type the CORRECT test PIN, press #", "GRANTED")
        st = cmd("status", r"fails=(\d+)")
        check(st[1] == "0", f"failure counter is {st[1]} after a valid card + PIN")
        intr.expect(r"\(simulated\) bolt thrown", 20, mi)
        return "valid card + PIN after failures: GRANTED, failure counter back to 0, relocked"

    def s_alarm():
        m = intr.mark()
        me = ext.mark()
        cmd("door open", r"\[sim\] door OPEN")
        intr.expect(r"\[alarm\] RAISED: FORCED_DOOR", 3, m)
        ext.expect(r"\[status\] lock=\d+ alarm=1 ", 5, me)
        confirm("OLED top line shows '!! ALARM !!', the exterior RED LED blinks fast, and the interior siren sounds (if wired)?")
        cmd("door close", r"\[sim\] door closed")
        _, me2, mi, _ = card_and_pin("Tap the test card, type the test PIN, press # (this clears the alarm)", "GRANTED")
        intr.expect(r"\[alarm\] cleared by valid card \+ PIN", 3, mi)
        ext.expect(r"\[status\] lock=\d+ alarm=0 ", 8, me2)
        intr.expect(r"\(simulated\) bolt thrown", 20, mi)
        return "forced door while locked: alarm shown on the exterior; cleared by card + PIN"

    def s_lockout():
        say("Enter WRONG PINs with the test card until the screen says LOCKED OUT (5 failures in total)")
        me, mi = ext.mark(), intr.mark()
        r = ext.expect(r"\[auth\] result LOCKED_OUT attempts_left=\d+ lockout=(\d+)s", WAIT * 5, me)
        tries = len(ext.find(r"\[auth\] result DENIED", me)) + 1
        ext.expect(r"-> Lockout", 3, me)
        confirm("OLED shows 'LOCKED OUT' with a countdown, and the YELLOW LED blinks slowly?")
        say(f"Wait for the countdown ({r[1]} s). Do not tap cards.")
        ext.expect(r"Lockout -> CardWait", int(r[1]) + 10, me)
        _, _, mi2, _ = card_and_pin("Tap the test card, type the CORRECT test PIN, press #", "GRANTED")
        intr.expect(r"\(simulated\) bolt thrown", 20, mi2)
        return f"locked out after {tries} wrong PIN(s) for {r[1]} s; countdown ended; GRANTED afterwards"

    def s_linkloss():
        me = ext.mark()
        intr.hold_reset(True)
        ext.expect(r"interior silent, dropping session", 15, me)
        ext.expect(r"-> Linking", 3, me)
        confirm("OLED shows 'DOOR LOCK / CONNECTING' with the YELLOW LED on?")
        me2 = ext.mark()
        intr.hold_reset(False)
        ext.expect(r"\[link\] session confirmed", 25, me2)
        ext.expect(r"Linking -> CardWait", 5, me2)
        return "interior power loss: exterior shows CONNECTING, recovers when the interior returns"

    def s_integrity():
        bad = intr.find(r"failed authentication") + ext.find(r"failed authentication")
        check(not bad, f"{len(bad)} frame(s) failed authentication")
        to = ext.find(r"result TIMEOUT")
        check(not to, f"{len(to)} request(s) got no answer from the interior")
        keys = len(ext.find(r"\[key\] "))
        return f"0 failed frames, 0 unanswered requests, {keys} key presses logged"

    steps = [
        ("boot + link", s_boot, None),
        ("interior prep", s_prep, None),
        ("idle screen", s_idle_screen, None),
        ("enroll card + PIN", s_enroll, None),
        ("valid card + PIN", s_grant, None),
        ("door open / close", s_door_cycle, None),
        ("cancel with *", s_cancel, None),
        ("PIN too short", s_short_pin, None),
        ("PIN entry timeout", s_timeout, None),
        ("wrong PIN", s_wrong_pin, None),
        ("unknown card", s_unknown, "unknown"),
        ("counter reset", s_reset_after_denied, None),
        ("alarm on exterior", s_alarm, None),
        ("lockout", s_lockout, "lockout"),
        ("interior link loss", s_linkloss, "linkloss"),
        ("frame integrity", s_integrity, None),
    ]
    results, failed = [], False
    for title, fn, opt in steps:
        if opt and opt in skip:
            results.append((title, "SKIP", "skipped (--skip)", 0))
            continue
        print(f"\n--- {title}", flush=True)
        t0 = time.time()
        try:
            note, res = fn(), "PASS"
        except Fail as e:
            note, res, failed = str(e), "FAIL", True
        except (KeyboardInterrupt, EOFError):
            note, res, failed = "stopped by operator", "FAIL", True
        except Exception as e:   # a script bug must not skip the cleanup
            note, res, failed = f"test script error: {type(e).__name__}: {e}", "FAIL", True
        results.append((title, res, note, time.time() - t0))
        print(f"  {res}  {title}: {note}", flush=True)
        if res == "FAIL":
            break

    try:
        intr.hold_reset(False)
        time.sleep(0.5)
        intr.send("door close")
        if state["slot"] is not None:
            cmd(f"delete {state['slot']}", r"deleted|no such slot", 5)
        cmd("resetlockout", r"lockout reset", 5)
        print(f"\n  cleanup: test card (slot {state['slot']}) deleted, lockout reset")
    except Fail as e:
        print(f"\n  cleanup FAILED: {e}  (delete the test card by hand: 'list', 'delete <slot>')")

    stamp = time.strftime("%Y%m%d-%H%M%S")
    logname = f"flow_test_{stamp}.log"
    with open(logname, "w") as f:
        for title, res, note, dt in results:
            f.write(f"{res:4}  {title:20} {dt:6.1f} s  {note}\n")
        for b in (intr, ext):
            f.write(f"\n===== {b.name} serial log =====\n")
            f.write("\n".join(b.lines) + "\n")

    print("\n================ FLOW TEST SUMMARY ================")
    for title, res, note, dt in results:
        print(f"{res:4}  {title:20} {dt:6.1f} s  {note}")
    run = [r for r in results if r[1] != "SKIP"]
    print(f"---- {sum(r[1] == 'PASS' for r in run)}/{len(run)} passed"
          + (f", stopped after the first failure ({len(steps) - len(results)} not run)" if len(results) < len(steps) else ""))
    print(f"logs saved to {logname}")
    print("Flash env:interior again before fitting the board to a door.")
    intr.close()
    ext.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
