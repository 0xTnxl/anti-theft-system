#!/usr/bin/env python3
"""Get (or renew) the door's Let's Encrypt certificate for https://door.usezend.app.

The door keeps its private key. This tool takes the door's certificate request
(CSR), has Let's Encrypt sign it (DNS-01 through the name.com API, so nothing
on the door or the router is opened up), and gives the certificate back to the
door. Let's Encrypt certificates last 90 days (64 from February 2027, 45 from
2028), so run this about every 6 weeks in 2027 and monthly after. The app warns
the admin two weeks before expiry.

    export NAMECOM_USER=... NAMECOM_TOKEN=... ACME_EMAIL=...

    # PC connected to the door by USB and to the internet: one step
    python3 tools/renew_cert.py renew --serial auto

    # Or over WiFi, in three steps (the door's WiFi has no internet):
    python3 tools/renew_cert.py csr            # on ATDS-Door: saves dist/door.csr
    python3 tools/renew_cert.py sign           # on the internet: dist/door-fullchain.pem
    python3 tools/renew_cert.py upload         # on ATDS-Door again
    # (the app can also upload the .pem: Settings > Secure address)

Add --staging to try the whole flow with Let's Encrypt's test CA first (phones
will not trust that certificate; it avoids the production rate limits).
acme.sh is installed for this user in ~/.zend-acme (no sudo needed).
"""
import argparse
import getpass
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from door_client import HOST, ROOT, Door, DoorError  # noqa: E402

DIST = os.path.join(ROOT, "dist")
CSR = os.path.join(DIST, "door.csr")
CHAIN = os.path.join(DIST, "door-fullchain.pem")
ACME_HOME = os.path.expanduser("~/.zend-acme")
ACME = os.path.join(ACME_HOME, "acme.sh")


# ------------------------------------------------------------- the door ---

def open_serial(port):
    import serial  # pyserial, in PlatformIO's Python
    if port == "auto":
        out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "find_port.py"), "interior"],
                             capture_output=True, text=True)
        port = out.stdout.strip().splitlines()[-1] if out.returncode == 0 and out.stdout.strip() else ""
        if not port:
            sys.exit("interior board not found on USB (tools/find_port.py interior)")
    s = serial.Serial(port, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
    s.dtr = s.rts = False
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def serial_lines(s, until, timeout=10):
    end, lines = time.time() + timeout, []
    while time.time() < end:
        line = s.readline().decode(errors="replace").rstrip("\r\n")
        if not line:
            continue
        lines.append(line)
        if until(line):
            return lines
    sys.exit("no answer from the door on USB:\n" + "\n".join(lines[-5:]))


def get_csr(args):
    if args.serial:
        s = open_serial(args.serial)
        s.write(b"cert csr\n")
        lines = serial_lines(s, lambda l: "END CERTIFICATE REQUEST" in l)
        start = next(i for i, l in enumerate(lines) if "BEGIN CERTIFICATE REQUEST" in l)
        pem = "\n".join(lines[start:]) + "\n"
    else:
        door = login(args)
        r = door.call("GET", "/api/cert")
        if "csr" not in r:
            sys.exit(f"the door did not give a certificate request: {r.get('msg', r)}")
        pem = r["csr"]
        show(r)
    os.makedirs(DIST, exist_ok=True)
    open(CSR, "w").write(pem)
    print(f"saved the door's certificate request: {CSR}")


def upload(args):
    pem = open(CHAIN).read()
    if pem.count("BEGIN CERTIFICATE") < 2:
        sys.exit(f"{CHAIN} is not a full chain")
    if args.serial:
        s = open_serial(args.serial)
        s.write(b"cert begin\n")
        serial_lines(s, lambda l: "paste the full-chain" in l)
        for line in pem.strip().splitlines():
            s.write(line.strip().encode() + b"\n")
            serial_lines(s, lambda l: l == "ok" or "FAILED" in l, 5)
        s.write(b"cert end\n")
        result = serial_lines(s, lambda l: "[cert] OK" in l or "[cert] FAILED" in l)[-1]
        print(result)
        return 0 if "OK" in result else 1
    door = login(args)
    r = door.call("POST", "/api/cert", pem.encode(), "application/x-pem-file")
    print(("OK: " if r.get("ok") else "FAILED: ") + str(r.get("msg")))
    return 0 if r.get("ok") else 1


def login(args):
    door = Door(args.host)
    try:
        door.login(os.environ.get("ATDS_ADMIN_PASSWORD") or getpass.getpass("App admin password: "))
    except DoorError as e:
        sys.exit(str(e))
    return door


def show(r):
    if r.get("installed"):
        left = (r["notAfter"] - time.time()) / 86400
        print(f"door has a certificate from {r.get('issuer')}{' (STAGING)' if r.get('staging') else ''}, "
              f"{left:.0f} days left")
    else:
        print("door has no certificate for door.usezend.app yet")


# ------------------------------------------------------- Let's Encrypt ---

def need(name):
    v = os.environ.get(name)
    if not v:
        sys.exit(f"set {name} first (see the top of this file)")
    return v


def acme(*a, ok=(0,)):
    r = subprocess.run([ACME, "--home", ACME_HOME, *a])
    if r.returncode not in ok:
        sys.exit(f"acme.sh failed ({r.returncode})")
    return r.returncode


def sign(args):
    user, token, email = need("NAMECOM_USER"), need("NAMECOM_TOKEN"), need("ACME_EMAIL")
    if not os.path.exists(CSR):
        sys.exit(f"{CSR} missing: run 'csr' first")
    if not os.path.exists(ACME):
        print("== installing acme.sh into", ACME_HOME)
        sh = subprocess.run(["curl", "-fsSL", "https://raw.githubusercontent.com/acmesh-official/acme.sh/master/acme.sh"],
                            capture_output=True, check=True).stdout
        subprocess.run(["sh", "-s", "--", "--install-online", "--home", ACME_HOME, "--nocron", "--noprofile",
                        "-m", email], input=sh, check=True)
    os.environ["Namecom_Username"], os.environ["Namecom_Token"] = user, token
    server = "letsencrypt_test" if args.staging else "letsencrypt"
    acme("--register-account", "-m", email, "--server", server)
    print(f"== asking Let's Encrypt{' (staging)' if args.staging else ''} to sign the door's request for {HOST}")
    extra = ["--force"] if args.force else []
    code = acme("--sign-csr", "--csr", CSR, "--dns", "dns_namecom", "--server", server, *extra, ok=(0, 2))
    if code == 2:
        print("acme.sh: the current certificate is not due for renewal yet (use --force to renew anyway)")
    found = sorted(glob.glob(os.path.join(ACME_HOME, HOST + "*", "fullchain.cer")), key=os.path.getmtime)
    if not found:
        sys.exit("acme.sh finished but no fullchain.cer was found")
    os.makedirs(DIST, exist_ok=True)
    open(CHAIN, "w").write(open(found[-1]).read())
    print(f"saved the signed certificate chain: {CHAIN}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("step", choices=["csr", "sign", "upload", "renew", "status"])
    ap.add_argument("--serial", help="talk to the door over USB instead of WiFi ('auto' finds the interior)")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--staging", action="store_true", help="Let's Encrypt test CA")
    ap.add_argument("--force", action="store_true", help="renew even if not due")
    args = ap.parse_args()
    if args.step == "status":
        show(login(args).call("GET", "/api/cert"))
        return 0
    if args.step in ("csr", "renew"):
        get_csr(args)
    if args.step in ("sign", "renew"):
        sign(args)
    if args.step in ("upload", "renew"):
        return upload(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
