#!/usr/bin/env python3
"""End-to-end test of the exterior <-> interior secure link on two USB-connected boards.

Flash first (ports picked by board MAC):
    pio run -e interior      -t upload --upload-port $(python3 tools/find_port.py interior)
    pio run -e exterior_test -t upload --upload-port $(python3 tools/find_port.py exterior)
Run with PlatformIO's Python (it has pyserial):
    ~/.platformio/penv/bin/python tools/link_test.py [-v]

env:exterior_test adds a USB-serial "auth <uid hex> <pin>" command that stands in
for the RC522 and keypad, and rekeys every 60 s instead of 10 min. It must never
ship: flash env:exterior again afterwards.

The test enrolls a test card on the interior and wipes all cards at the end, so it
refuses to start if the interior already has cards (override with --force).
If the interior is wired to a door that is locked, GRANTED steps move the bolt.
Takes about 3 minutes.
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python")

HERE = os.path.dirname(os.path.abspath(__file__))
UID, PIN = "04A1B2C3", "4826"
OTHER_UID, WRONG_PIN = "DEADBEEF", "1111"


class Fail(Exception):
    pass


class Board:
    def __init__(self, name, port, verbose):
        self.name, self.verbose, self.lines = name, verbose, []
        self.lock = threading.Lock()
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.1
        s.dtr = s.rts = False          # do not reset on open
        s.open()
        self.s, self.running = s, True
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while self.running:
            try:
                chunk = self.s.read(256)
            except (serial.SerialException, OSError):
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                if text:
                    with self.lock:
                        self.lines.append(text)
                    if self.verbose:
                        print(f"      {self.name} | {text}")

    def mark(self):
        with self.lock:
            return len(self.lines)

    def send(self, cmd):
        self.s.write((cmd + "\n").encode())

    def expect(self, pattern, timeout, since):
        rx, end = re.compile(pattern), time.time() + timeout
        while True:
            with self.lock:
                tail = self.lines[since:]
            for line in tail:
                m = rx.search(line)
                if m:
                    return m
            if time.time() > end:
                raise Fail(f"{self.name}: no line matching /{pattern}/ within {timeout:.0f} s")
            time.sleep(0.05)

    def find(self, pattern, since=0):
        rx = re.compile(pattern)
        with self.lock:
            return [l for l in self.lines[since:] if rx.search(l)]

    def reset(self):
        self.s.rts = True
        time.sleep(0.1)
        self.s.rts = False

    def hold_reset(self, on):
        self.s.rts = on

    def close(self):
        self.running = False
        time.sleep(0.2)
        self.s.close()


def port_of(role):
    r = subprocess.run([sys.executable, os.path.join(HERE, "find_port.py"), role],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"cannot find the {role} board: {r.stderr.strip()}")
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description="ESP-to-ESP secure link test")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo every serial line")
    ap.add_argument("--force", action="store_true", help="run even if the interior has cards (they get wiped)")
    args = ap.parse_args()

    pi, pe = port_of("interior"), port_of("exterior")
    print(f"interior {pi}, exterior {pe}")
    intr, ext = Board("INT", pi, args.verbose), Board("EXT", pe, args.verbose)
    state = {"may_wipe": False}

    def check(cond, msg):
        if not cond:
            raise Fail(msg)

    def status():
        m = intr.mark()
        intr.send("status")
        a = intr.expect(r"link=(\w+) lastRx=(\d+)s ago", 3, m)
        c = intr.expect(r"cards=(\d+)/\d+ fails=(\d+) lockout=(\d+)s", 3, m)
        return {"link": a[1], "last_rx": int(a[2]), "cards": int(c[1]), "fails": int(c[2])}

    def auth(uid, pin, want, attempts=None):
        deadline = time.time() + 15
        while True:
            m, t0 = ext.mark(), time.time()
            ext.send(f"auth {uid} {pin}")
            if ext.expect(r"\[test\] (auth sent|not ready|bad|usage)", 3, m)[1] == "auth sent":
                break
            check(time.time() < deadline, "exterior never became ready to send")
            time.sleep(0.5)
        r = ext.expect(r"\[auth\] result (\w+) attempts_left=(\d+) lockout=(\d+)s", 8, m)
        ms = int((time.time() - t0) * 1000)
        check(r[1] == want, f"expected {want}, got {r[1]}")
        if attempts is not None:
            check(int(r[2]) == attempts, f"expected {attempts} attempts left, got {r[2]}")
        time.sleep(1.2)   # interior accepts one attempt per second
        return {"ms": ms, "attempts": int(r[2]), "lockout": int(r[3])}

    def s_boot():
        mi, me = intr.mark(), ext.mark()
        t0 = time.time()
        intr.reset()
        ext.reset()
        try:
            ext.expect(r"TEST BUILD", 10, me)
        except Fail:
            raise Fail("exterior is not running env:exterior_test (no TEST BUILD banner)")
        intr.expect(r"\[link\] session established", 20, mi)
        ext.expect(r"\[link\] session confirmed", 20, me)
        return f"both boards reset, encrypted session up after {time.time() - t0:.1f} s"

    def s_clean():
        st = status()
        check(st["cards"] == 0 or args.force,
              f"interior already has {st['cards']} card(s); refusing to wipe them (use --force)")
        state["may_wipe"] = True
        m = intr.mark()
        intr.send("resetlockout")
        intr.expect(r"lockout reset", 3, m)
        return f"{st['cards']} card(s) before test, failure counter reset"

    def s_heartbeat():
        time.sleep(4.5)
        st = status()
        check(st["link"] == "up" and st["last_rx"] <= 3, f"link={st['link']} lastRx={st['last_rx']} s")
        return f"heartbeats arriving (last frame {st['last_rx']} s ago)"

    def s_enroll():
        m = intr.mark()
        intr.send("enroll")
        intr.expect(r"\[enroll\] tap", 3, m)
        auth(UID, PIN, "ENROLL_AGAIN")
        intr.expect(r"step 1 ok", 3, m)
        auth(UID, PIN, "ENROLL_OK")
        g = intr.expect(r"stored in slot (\d+) \(PBKDF2 took (\d+) ms\)", 3, m)
        return f"card + PIN stored in slot {g[1]}, PBKDF2 hashing took {g[2]} ms"

    def s_grant():
        m = intr.mark()
        r = auth(UID, PIN, "GRANTED")
        intr.expect(r"\[auth\] GRANTED", 3, m)
        return f"GRANTED, request to answer {r['ms']} ms"

    def s_malformed():
        auth(UID, "12", "DENIED", attempts=5)
        return "2-digit PIN refused by the interior, not counted as a failure"

    def s_wrong_pin():
        m = intr.mark()
        auth(UID, WRONG_PIN, "DENIED", attempts=4)
        intr.expect(r"DENIED \(wrong PIN\)", 3, m)
        return "right card, wrong PIN: DENIED, 4 attempts left"

    def s_unknown():
        m = intr.mark()
        auth(OTHER_UID, PIN, "DENIED", attempts=3)
        intr.expect(r"DENIED \(unknown card\)", 3, m)
        return "unknown card: DENIED, 3 attempts left"

    def s_lockout():
        auth(UID, WRONG_PIN, "DENIED", attempts=2)
        auth(UID, WRONG_PIN, "DENIED", attempts=1)
        r = auth(UID, WRONG_PIN, "LOCKED_OUT")
        check(r["lockout"] > 0, "lockout time is 0")
        return f"5th failure: LOCKED_OUT for {r['lockout']} s"

    def s_reboot_lockout():
        mi, me = intr.mark(), ext.mark()
        intr.reset()
        g = intr.expect(r"lockout active after reboot: (\d+) s", 10, mi)
        ext.expect(r"interior silent, dropping session", 15, me)
        intr.expect(r"\[link\] session established", 20, mi)
        ext.expect(r"\[link\] session confirmed", 20, me)
        auth(UID, PIN, "LOCKED_OUT")
        return f"interior rebooted: lockout restored ({g[1]} s), link recovered, correct card + PIN still refused"

    def s_unlock_after():
        m = intr.mark()
        intr.send("resetlockout")
        intr.expect(r"lockout reset", 3, m)
        auth(UID, PIN, "GRANTED")
        check(status()["fails"] == 0, "failure counter not cleared by a valid card + PIN")
        return "lockout cleared on the interior; GRANTED; failure counter back to 0"

    def s_ext_loss():
        ext.hold_reset(True)
        time.sleep(12)
        st = status()
        mi, me = intr.mark(), ext.mark()
        ext.hold_reset(False)
        check(st["last_rx"] >= 10, f"interior lastRx only {st['last_rx']} s while exterior was held in reset")
        intr.expect(r"\[link\] session established", 25, mi)
        ext.expect(r"\[link\] session confirmed", 25, me)
        auth(UID, PIN, "GRANTED")
        return f"exterior held in reset 12 s (interior saw {st['last_rx']} s silence), new session after release, GRANTED"

    def s_rekey():
        mi, me, t0 = intr.mark(), ext.mark(), time.time()
        ext.expect(r"\[link\] session keys installed", 80, me)
        waited = time.time() - t0
        intr.expect(r"\[link\] session established", 5, mi)
        ext.expect(r"\[link\] session confirmed", 5, me)
        drops = ext.find(r"dropping session|not confirmed", me)
        check(not drops, f"link dropped during rekey: {drops[0] if drops else ''}")
        auth(UID, PIN, "GRANTED")
        return f"scheduled rekey after {waited:.0f} s of waiting, no drop, GRANTED on the new keys"

    def s_integrity():
        bad = intr.find(r"failed authentication") + ext.find(r"failed authentication")
        check(not bad, f"{len(bad)} frame(s) failed authentication: {bad[0] if bad else ''}")
        timeouts = ext.find(r"result TIMEOUT")
        check(not timeouts, f"{len(timeouts)} request(s) got no answer")
        return f"0 frames failed authentication over {len(intr.lines) + len(ext.lines)} log lines, 0 timeouts"

    steps = [
        ("boot + handshake", s_boot),
        ("interior clean", s_clean),
        ("heartbeat / status", s_heartbeat),
        ("enroll card + PIN", s_enroll),
        ("valid card + PIN", s_grant),
        ("malformed request", s_malformed),
        ("wrong PIN", s_wrong_pin),
        ("unknown card", s_unknown),
        ("lockout", s_lockout),
        ("interior reboot", s_reboot_lockout),
        ("lockout cleared", s_unlock_after),
        ("exterior power loss", s_ext_loss),
        ("rekey", s_rekey),
        ("frame integrity", s_integrity),
    ]
    results, failed = [], False
    for title, fn in steps:
        print(f"  ... {title}", flush=True)
        t0 = time.time()
        try:
            note, ok = fn(), True
        except Fail as e:
            note, ok, failed = str(e), False, True
        except Exception as e:   # a bug in this script must not skip the cleanup
            note, ok, failed = f"test script error: {type(e).__name__}: {e}", False, True
        results.append((title, ok, note, time.time() - t0))
        print(f"  {'PASS' if ok else 'FAIL'}  {title}: {note}", flush=True)
        if not ok:
            break

    if state["may_wipe"]:
        try:
            m = intr.mark()
            intr.send("wipe yes")
            intr.expect(r"all cards deleted", 3, m)
            intr.send("resetlockout")
            time.sleep(0.5)
            print(f"  cleanup: test card wiped, interior has {status()['cards']} card(s)")
        except Fail as e:
            print(f"  cleanup FAILED: {e}")

    print("\n================ LINK TEST SUMMARY ================")
    for title, ok, note, dt in results:
        print(f"{'PASS' if ok else 'FAIL'}  {title:22} {dt:5.1f} s  {note}")
    if len(results) < len(steps):
        print(f"(stopped after the first failure; {len(steps) - len(results)} step(s) not run)")
    print(f"---- {sum(r[1] for r in results)}/{len(steps)} passed")
    intr.close()
    ext.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
