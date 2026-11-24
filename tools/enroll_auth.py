#!/usr/bin/env python3
"""Register a real RFID card + PIN, then authenticate with it end to end.

Real parts: RC522 card read and keypad on the exterior, the secure ESP-NOW link,
and the interior's credential check. Works without the OLED (watch this terminal
and the exterior LEDs instead).

    ~/.platformio/penv/bin/python tools/enroll_auth.py            # enroll + 2 auth checks
    ~/.platformio/penv/bin/python tools/enroll_auth.py --auth-only  # card already enrolled
    add --cleanup to delete the card again at the end

Firmware: env:exterior on the exterior; env:interior_sim (preferred, no motor) or
env:interior on the interior. With env:interior and no motor fitted, a GRANTED
unlock ends in a motor FAULT; the access decision is still tested.

At the exterior: tap the card (hold it flat ~1 s), type the PIN (4-8 digits), press #.
'*' clears the digits. Each step waits up to 90 s.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from link_test import Board, Fail, port_of  # noqa: E402

WAIT = 90


def main():
    ap = argparse.ArgumentParser(description="enroll a card + PIN, then authenticate")
    ap.add_argument("--auth-only", action="store_true", help="skip enrollment")
    ap.add_argument("--cleanup", action="store_true", help="delete the enrolled card at the end")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo every serial line")
    args = ap.parse_args()

    pi, pe = port_of("interior"), port_of("exterior")
    intr, ext = Board("INT", pi, args.verbose), Board("EXT", pe, args.verbose)
    results, slot = [], None

    def say(t):
        print(f"\n  >>> {t}", flush=True)

    def step(name, ok, note):
        results.append((name, ok, note))
        print(f"  {'PASS' if ok else 'FAIL'}  {name}: {note}", flush=True)

    def tap_and_pin(prompt, on_card=None):
        """Waits for one full card + PIN attempt. Returns (code, seconds, rfid_drops)."""
        me = ext.mark()
        say(prompt)
        ext.expect(r"\[rfid\] card read, (\d+)-byte UID", WAIT, me)
        if on_card:
            on_card()
        print("      card read, now type the PIN and press #  (15 s without a key cancels)", flush=True)
        t0 = time.time()
        # Echo keypad activity live: the OLED may be missing, so this is the only feedback.
        seen, iseen, end, r = ext.mark(), intr.mark(), time.time() + WAIT, None
        rx = re.compile(r"\[auth\] result (\w+) attempts_left=(\d+) lockout=(\d+)s")
        while r is None:
            for line in ext.lines[seen:]:
                if line.startswith(("[key]", "[ui]")):
                    print(f"      exterior: {line}", flush=True)
                if "PIN entry timed out" in line:
                    print("      !! no key for 15 s: back to tap-card. Tap the card again.", flush=True)
                r = r or rx.search(line)
            seen = len(ext.lines)
            for line in intr.lines[iseen:]:
                if line.startswith(("[enroll]", "[auth]", "[lock]", "[alarm]")):
                    print(f"      interior: {line}", flush=True)
            iseen = len(intr.lines)
            if r is None and time.time() > end:
                keys = len(ext.find(r"\[key\] ", me))
                raise Fail(f"no PIN submitted within {WAIT} s ({keys} key presses detected"
                           f"{': keypad not read, run the exterior kick test keys check' if not keys else ''})")
            time.sleep(0.05)
        # The interior logs its reason just after answering: show it.
        end2 = time.time() + 1.5
        while time.time() < end2:
            for line in intr.lines[iseen:]:
                if line.startswith(("[enroll]", "[auth]", "[lock]", "[alarm]")):
                    print(f"      interior: {line}", flush=True)
            iseen = len(intr.lines)
            time.sleep(0.05)
        drops = len(ext.find(r"RC522 not responding", me))
        return r[1], int(r[2]), time.time() - t0, drops

    try:
        # Boot both, wait for the link and the reader.
        mi, me = intr.mark(), ext.mark()
        intr.reset()
        ext.reset()
        try:
            intr.expect(r"\[int\] anti-theft interior module", 10, mi)
        except Fail:
            other = intr.find(r"\[kick\]", mi)
            raise Fail("interior is running the kick test, not the interior firmware. Flash it first:\n"
                       "    pio run -e interior_sim -t upload --upload-port $(python3 tools/find_port.py interior)"
                       if other else "no interior firmware banner: is env:interior_sim (or env:interior) flashed?")
        time.sleep(1)
        sim = bool(intr.find(r"SIMULATED BOLT", mi))
        intr.expect(r"\[link\] session established", 20, mi)
        try:
            ext.expect(r"\[ext\] anti-theft exterior module", 10, me)
        except Fail:
            raise Fail("exterior is not running env:exterior (kick test or other build?). Flash it:\n"
                       "    pio run -e exterior -t upload --upload-port $(python3 tools/find_port.py exterior)")
        ext.expect(r"\[link\] session confirmed", 20, me)
        ext.expect(r"Linking -> CardWait", 10, me)
        oled = not ext.find(r"SSD1306 not found", me)
        try:
            ext.expect(r"\[rfid\] RC522 ready", 10, me)
            rfid = "RC522 found"
        except Fail:
            rfid = "RC522 NOT found yet (reads may fail)"
        step("link", True, f"session up; interior {'simulated bolt' if sim else 'REAL motor build'}; "
                           f"OLED {'found' if oled else 'missing'}; {rfid}")
        print("  Exterior LEDs: yellow = typing PIN / waiting, green = granted, red = denied.")

        if not args.auth_only:
            def start_enroll():
                # Started when the card is read, so a slow card tap cannot use up
                # the interior's enroll window before the PIN arrives.
                m = intr.mark()
                intr.send("enroll")
                intr.expect(r"\[enroll\] tap", 3, m)
            code, _, _, d1 = tap_and_pin("ENROLL 1/2: tap the card, type the new PIN, press #", start_enroll)
            if code != "ENROLL_AGAIN":
                raise Fail(f"expected ENROLL_AGAIN, got {code}")
            m = intr.mark()
            code, _, _, d2 = tap_and_pin("ENROLL 2/2: tap the SAME card, type the SAME PIN, press #")
            if code != "ENROLL_OK":
                raise Fail(f"expected ENROLL_OK, got {code} (card or PIN differed from step 1?)")
            g = intr.expect(r"stored in slot (\d+) \(PBKDF2 took (\d+) ms\)", 3, m)
            slot = int(g[1])
            step("enroll", True, f"card + PIN stored in slot {slot} (hash {g[2]} ms); reader dropouts {d1 + d2}")
            time.sleep(2)

        mi = intr.mark()
        code, _, secs, d = tap_and_pin("AUTH: tap the enrolled card, type its PIN, press #")
        if code != "GRANTED":
            raise Fail(f"expected GRANTED, got {code}")
        intr.expect(r"\[auth\] GRANTED", 3, mi)
        try:
            intr.expect(r"\(simulated\) bolt retracted|-> UNLOCKED|FAULT", 5, mi)
        except Fail:
            pass
        lock = [l for l in intr.find(r"\[lock\]", mi)]
        step("auth (correct PIN)", True, f"GRANTED {secs:.1f} s after the card read; interior: "
                                         f"{'; '.join(lock[-2:]) or 'no lock change'}")

        time.sleep(3)
        mi = intr.mark()
        code, left, _, _ = tap_and_pin("AUTH: tap the same card, type a WRONG PIN, press #")
        if code != "DENIED":
            raise Fail(f"expected DENIED, got {code}")
        intr.expect(r"DENIED \(wrong PIN\)", 3, mi)
        step("auth (wrong PIN)", True, f"DENIED, {left} attempts left")

        bad = intr.find(r"failed authentication") + ext.find(r"failed authentication")
        step("link integrity", not bad, f"{len(bad)} frames failed authentication")
    except Fail as e:
        step("stopped", False, str(e))
    except KeyboardInterrupt:
        step("stopped", False, "Ctrl+C")
    finally:
        try:
            m = intr.mark()
            intr.send("resetlockout")
            intr.expect(r"lockout reset", 3, m)
            if args.cleanup and slot is not None:
                m = intr.mark()
                intr.send(f"delete {slot}")
                intr.expect(r"deleted", 3, m)
                print(f"  cleanup: slot {slot} deleted")
        except Fail:
            pass
        drops = len(ext.find(r"RC522 not responding"))
        print("\n================ ENROLL + AUTH SUMMARY ================")
        for n, ok, note in results:
            print(f"{'PASS' if ok else 'FAIL'}  {n:20} {note}")
        if drops:
            print(f"note: RC522 dropped out {drops} time(s) during the run: check its wiring and 3.3 V supply")
        intr.close()
        ext.close()
    return 0 if results and all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
