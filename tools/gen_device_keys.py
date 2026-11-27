#!/usr/bin/env python3
"""Create the HTTPS certificates and the OTA signing key for the interior board.

    python3 tools/gen_device_keys.py              # create what is missing
    python3 tools/gen_device_keys.py --renew-server   # new HTTPS certificate, same CA (phones keep working)
    python3 tools/gen_device_keys.py --new-ca     # new CA: every phone must install the new ca.crt

Writes (all git-ignored):
  keys/ca.key, keys/ca.crt         local certificate authority. ca.crt is installed on each phone
  keys/server.key, keys/server.crt HTTPS certificate for https://192.168.4.1 (825 days, Apple's limit)
  keys/ota_signing.pem             OTA private key. KEEP SAFE: whoever has it can update the door
  include/tls_secrets.h            server certificate + key + CA (DER) compiled into the firmware
  include/ota_pubkey.h             OTA public key compiled into the firmware

Rebuild and flash (USB or OTA) after any change.
"""
import argparse
import datetime
import ipaddress
import os
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
KEYS = os.path.join(ROOT, "keys")
INC = os.path.join(ROOT, "include")
AP_IP = "192.168.4.1"


def p(name):
    return os.path.join(KEYS, name)


def write_private(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)


def save_key(key, path):
    write_private(path, key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))


def load_key(path):
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), None)


def load_cert(path):
    with open(path, "rb") as f:
        return x509.load_pem_x509_certificate(f.read())


def now():
    return datetime.datetime.utcnow() - datetime.timedelta(minutes=5)


def make_ca():
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "ATDS Door local CA"),
                      x509.NameAttribute(NameOID.ORGANIZATION_NAME, "ATDS Door")])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now())
            .not_valid_after(now() + datetime.timedelta(days=3650))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
            .sign(key, hashes.SHA256()))
    save_key(key, p("ca.key"))
    with open(p("ca.crt"), "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    print("created keys/ca.crt (install this on each phone)")


def make_server():
    ca_key, ca = load_key(p("ca.key")), load_cert(p("ca.crt"))
    key = ec.generate_private_key(ec.SECP256R1())
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, AP_IP)]))
            .issuer_name(ca.subject).public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now()).not_valid_after(now() + datetime.timedelta(days=825))
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address(AP_IP)),
                                                        x509.DNSName("atds-door.local")]), critical=False)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
            .sign(ca_key, hashes.SHA256()))
    save_key(key, p("server.key"))
    with open(p("server.crt"), "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    print(f"created keys/server.crt for https://{AP_IP} (valid until {cert.not_valid_after:%Y-%m-%d})")


def c_string(name, pem):
    lines = pem.decode().strip().splitlines()
    body = "\n".join(f'    "{l}\\n"' for l in lines)
    return f"static const char {name}[] =\n{body};\n"


def c_bytes(name, data):
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f"static const unsigned char {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "};\n"


def write_headers():
    srv = open(p("server.crt"), "rb").read()
    key = open(p("server.key"), "rb").read()
    ca_der = load_cert(p("ca.crt")).public_bytes(serialization.Encoding.DER)
    with open(os.path.join(INC, "tls_secrets.h"), "w") as f:
        f.write("// GENERATED by tools/gen_device_keys.py. Do not commit. Do not edit.\n#pragma once\n\n"
                "// sizeof() includes the terminating NUL, which mbedTLS needs for PEM input.\n")
        f.write(c_string("TLS_SERVER_CERT", srv) + "\n" + c_string("TLS_SERVER_KEY", key) + "\n")
        f.write("// Local CA certificate (DER), served at http://192.168.4.1/ca.crt for phones to install.\n")
        f.write(c_bytes("TLS_CA_DER", ca_der))
    os.chmod(os.path.join(INC, "tls_secrets.h"), 0o600)
    pub = load_key(p("ota_signing.pem")).public_key().public_bytes(serialization.Encoding.PEM,
                                                                     serialization.PublicFormat.SubjectPublicKeyInfo)
    with open(os.path.join(INC, "ota_pubkey.h"), "w") as f:
        f.write("// GENERATED by tools/gen_device_keys.py. Public key that OTA updates must be signed with.\n"
                "#pragma once\n\n" + c_string("OTA_PUBKEY_PEM", pub))
    print("wrote include/tls_secrets.h and include/ota_pubkey.h")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--renew-server", action="store_true", help="new HTTPS certificate, same CA")
    ap.add_argument("--new-ca", action="store_true", help="new CA (phones must reinstall ca.crt)")
    args = ap.parse_args()
    os.makedirs(KEYS, exist_ok=True)
    os.chmod(KEYS, 0o700)
    if args.new_ca or not os.path.exists(p("ca.key")):
        make_ca()
        args.renew_server = True
    if args.renew_server or not os.path.exists(p("server.key")):
        make_server()
    if not os.path.exists(p("ota_signing.pem")):
        save_key(ec.generate_private_key(ec.SECP256R1()), p("ota_signing.pem"))
        print("created keys/ota_signing.pem (KEEP SAFE: it authorises firmware updates)")
    write_headers()
    return 0


if __name__ == "__main__":
    sys.exit(main())
