#!/usr/bin/env python3
"""Upload a signed OTA package to the interior over HTTPS.

    python3 tools/ota_upload.py dist/interior-v2.atds          # asks for the app admin password
    python3 tools/ota_upload.py dist/interior-v2.atds --host 192.168.4.1

The PC must be on the ATDS-Door WiFi (or reach the door through the bridge).
The connection is checked against keys/ca.crt. The dashboard (Settings >
Software update) does the same from a phone.
"""
import argparse
import getpass
import http.cookiejar
import json
import os
import ssl
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("package")
    ap.add_argument("--host", default="192.168.4.1")
    args = ap.parse_args()
    data = open(args.package, "rb").read()
    ctx = ssl.create_default_context(cafile=os.path.join(ROOT, "keys", "ca.crt"))
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx), urllib.request.HTTPCookieProcessor(jar))
    base = f"https://{args.host}"
    hdr = {"X-ATDS": "1"}

    def call(path, body, ctype, timeout=30):
        req = urllib.request.Request(base + path, data=body, headers=dict(hdr, **{"Content-Type": ctype}))
        try:
            with op.open(req, timeout=timeout) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return json.loads(e.read() or b"{}")

    pw = getpass.getpass("App admin password: ")
    r = call("/api/login", urllib.parse.urlencode({"password": pw}).encode(), "application/x-www-form-urlencoded")
    if not r.get("ok"):
        sys.exit(f"login failed: {r.get('msg')}")
    print(f"uploading {len(data)} bytes...")
    t0 = time.time()
    r = call("/api/ota", data, "application/octet-stream", timeout=180)
    print(f"{'OK' if r.get('ok') else 'FAILED'}: {r.get('msg')} ({time.time() - t0:.0f} s)")
    return 0 if r.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
