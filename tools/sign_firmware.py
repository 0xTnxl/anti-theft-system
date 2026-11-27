#!/usr/bin/env python3
"""Build-output -> signed OTA package for the door.

    pio run -e interior
    python3 tools/sign_firmware.py interior            # -> dist/interior-v<N>.atds
    python3 tools/sign_firmware.py interior --env interior_bench

Package = 160-byte header + firmware.bin:
  0  magic "ATDSOTA1"      8   version (u32 LE)      12 image length (u32 LE)
  16 SHA-256 of the image  48  target name (16 bytes, zero padded)
  64 signature length      68  ECDSA P-256 signature (DER, max 72) over SHA-256(header[0:64])
The door checks the signature before writing anything, then the image hash
while it writes, and refuses a lower version than it runs or another target.
The version comes from include/version.h (bump it for every release).
"""
import argparse
import hashlib
import os
import re
import struct
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HEADER_LEN = 160


def version():
    m = re.search(r"#define\s+ATDS_FW_VERSION\s+(\d+)", open(os.path.join(ROOT, "include", "version.h")).read())
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target", choices=["interior", "exterior"], help="board the package is for")
    ap.add_argument("--env", help="PlatformIO env to take firmware.bin from (default: the target name)")
    args = ap.parse_args()
    env = args.env or args.target
    if not env.startswith(args.target):
        sys.exit(f"env {env} is not a {args.target} build")
    fw = os.path.join(ROOT, ".pio", "build", env, "firmware.bin")
    if not os.path.exists(fw):
        sys.exit(f"{fw} not found: run  pio run -e {env}  first")
    image = open(fw, "rb").read()
    if len(image) > 0x140000:
        sys.exit("image larger than an OTA slot (1.25 MB)")
    v = version()
    head = b"ATDSOTA1" + struct.pack("<II", v, len(image)) + hashlib.sha256(image).digest()
    head += args.target.encode().ljust(16, b"\0")
    assert len(head) == 64
    key = serialization.load_pem_private_key(open(os.path.join(ROOT, "keys", "ota_signing.pem"), "rb").read(), None)
    sig = key.sign(head, ec.ECDSA(hashes.SHA256()))
    header = (head + struct.pack("<B3x", len(sig)) + sig).ljust(HEADER_LEN, b"\0")
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", f"{args.target}-v{v}.atds")
    with open(out, "wb") as f:
        f.write(header + image)
    print(f"{out}: {args.target} firmware v{v} ({env}), {len(image)} bytes, signed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
