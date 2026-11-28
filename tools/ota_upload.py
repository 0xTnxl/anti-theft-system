#!/usr/bin/env python3
"""Upload a signed OTA package to the interior over HTTPS.

    python3 tools/ota_upload.py dist/interior-v2.atds          # asks for the app admin password
    python3 tools/ota_upload.py dist/interior-v2.atds --host 192.168.4.1

The PC must be on the ATDS-Door WiFi. The door's certificate is checked
(door.usezend.app, or keys/ca.crt before that is installed). The dashboard
(Settings > Software update) does the same from a phone.
"""
import argparse
import getpass
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from door_client import Door, DoorError  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("package")
    ap.add_argument("--host", default="192.168.4.1")
    args = ap.parse_args()
    data = open(args.package, "rb").read()
    door = Door(args.host)
    try:
        door.login(os.environ.get("ATDS_ADMIN_PASSWORD") or getpass.getpass("App admin password: "))
        print(f"uploading {len(data)} bytes...")
        t0 = time.time()
        r = door.call("POST", "/api/ota", data, "application/octet-stream", timeout=180)
    except DoorError as e:
        sys.exit(str(e))
    print(f"{'OK' if r.get('ok') else 'FAILED'}: {r.get('msg')} ({time.time() - t0:.0f} s)")
    return 0 if r.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
