#!/usr/bin/env python3
"""Offline test of zend_bridge.py against a fake door on localhost.

    python3 tools/bridge/test_bridge.py

Starts a fake door (using the real keys/server.crt, verified against
keys/ca.crt like the real door) and the bridge with a throwaway "public" CA,
runs the checks, then stops both. Nothing touches the real boards or the network.
"""
import datetime
import http.client
import http.server
import json
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HOST = "door.usezend.app"
fails = 0


def check(name, cond, info=""):
    global fails
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {info}")
    fails += not cond


def make_public_cert(d):
    n = datetime.datetime.utcnow() - datetime.timedelta(minutes=5)
    ck = ec.generate_private_key(ec.SECP256R1())
    cn = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "test public CA")])
    ca = (x509.CertificateBuilder().subject_name(cn).issuer_name(cn).public_key(ck.public_key()).serial_number(1)
          .not_valid_before(n).not_valid_after(n + datetime.timedelta(days=1))
          .add_extension(x509.BasicConstraints(ca=True, path_length=None), True).sign(ck, hashes.SHA256()))
    k = ec.generate_private_key(ec.SECP256R1())
    c = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, HOST)]))
         .issuer_name(cn).public_key(k.public_key()).serial_number(2).not_valid_before(n)
         .not_valid_after(n + datetime.timedelta(days=1))
         .add_extension(x509.SubjectAlternativeName([x509.DNSName(HOST)]), False).sign(ck, hashes.SHA256()))
    open(f"{d}/pubca.pem", "wb").write(ca.public_bytes(serialization.Encoding.PEM))
    open(f"{d}/full.pem", "wb").write(c.public_bytes(serialization.Encoding.PEM))
    open(f"{d}/key.pem", "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                                     serialization.NoEncryption()))


class FakeDoor(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    last = {}

    def log_message(self, *a):
        pass

    def out(self, code, obj, extra=()):
        b = json.dumps(obj).encode()
        self.send_response(code)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path != "/seen":
            FakeDoor.last = {"host": self.headers.get("Host"), "xff": self.headers.get("X-Forwarded-For")}
        if self.path == "/":
            b = b"<html>door app</html>"
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        elif self.path == "/seen":
            self.out(200, FakeDoor.last)
        elif self.path.startswith("/api/status"):
            ok = "atds=abc" in (self.headers.get("Cookie") or "")
            self.out(200 if ok else 401, {"lock": "LOCKED"} if ok else {"ok": False})
        else:
            self.out(404, {"ok": False})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        if self.headers.get("X-ATDS") != "1":
            return self.out(400, {"ok": False, "msg": "missing request header"})
        if self.path == "/api/login":
            return self.out(200, {"ok": True}, [("Set-Cookie", "atds=abc; Path=/; Secure; HttpOnly; SameSite=Strict")])
        if self.path == "/api/ota":
            return self.out(200, {"ok": True, "got": n})
        self.out(404, {})


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    d = tempfile.mkdtemp()
    make_public_cert(d)
    door_port, https_port, http_port = free_port(), free_port(), free_port()
    door = subprocess.Popen([sys.executable, __file__, "--fake-door", str(door_port)], stdout=subprocess.PIPE, text=True)
    door.stdout.readline()
    br = subprocess.Popen([sys.executable, f"{ROOT}/tools/bridge/zend_bridge.py", "--cert", f"{d}/full.pem", "--key",
                           f"{d}/key.pem", "--listen", "127.0.0.1", "--port", str(https_port), "--http-port",
                           str(http_port), "--door", "127.0.0.1", "--door-port", str(door_port), "--door-name",
                           "192.168.4.1", "--door-ca", f"{ROOT}/keys/ca.crt"], stdout=subprocess.PIPE, text=True)
    try:
        br.stdout.readline()
        ctx = ssl.create_default_context(cafile=f"{d}/pubca.pem")

        def req(m, p, body=None, h=None):
            c = http.client.HTTPSConnection("127.0.0.1", https_port, context=ctx)
            c.sock = ctx.wrap_socket(socket.create_connection(("127.0.0.1", https_port)), server_hostname=HOST)
            c.request(m, p, body=body, headers=h or {})
            r = c.getresponse()
            return r.status, dict(r.getheaders()), r.read()

        s, h, b = req("GET", "/")
        check("app page relayed", s == 200 and b.startswith(b"<html>door"))
        check("HSTS added", "max-age" in h.get("Strict-Transport-Security", ""))
        s, _, b = req("GET", "/seen")
        seen = json.loads(b)
        check("door sees its own Host and the client address", seen.get("host") == "192.168.4.1" and
              seen.get("xff") == "127.0.0.1", str(seen))
        s, h, b = req("POST", "/api/login", b"password=x", {"X-ATDS": "1", "Content-Type": "application/x-www-form-urlencoded"})
        check("login cookie passed back unchanged", s == 200 and "Secure; HttpOnly; SameSite=Strict" in h.get("Set-Cookie", ""))
        s, _, _ = req("GET", "/api/status")
        check("no cookie -> 401 from the door", s == 401)
        s, _, b = req("GET", "/api/status", None, {"Cookie": "atds=abc"})
        check("cookie forwarded -> status", s == 200 and json.loads(b)["lock"] == "LOCKED")
        s, _, b = req("POST", "/api/action", b"a=lock", {"Cookie": "atds=abc"})
        check("POST without X-ATDS still refused by the door", s == 400)
        blob = os.urandom(1_200_000)
        s, _, b = req("POST", "/api/ota", blob, {"X-ATDS": "1", "Cookie": "atds=abc", "Content-Type": "application/octet-stream"})
        check("1.2 MB OTA upload relayed intact", s == 200 and json.loads(b).get("got") == len(blob))
        c = http.client.HTTPConnection("127.0.0.1", http_port)
        c.request("GET", "/api/me")
        r = c.getresponse()
        check("http -> https redirect", r.status == 301 and r.getheader("Location") == f"https://{HOST}/api/me")
        door.kill()
        door.wait(5)
        time.sleep(0.3)
        s, _, b = req("GET", "/api/status", None, {"Cookie": "atds=abc"})
        check("door offline -> friendly 502", s == 502 and "not answering" in json.loads(b)["msg"])
    finally:
        if door.poll() is None:
            door.kill()
        br.terminate()
        br.wait(5)
    print("RESULT:", "all passed" if not fails else f"{fails} failed")
    return 1 if fails else 0


def fake_door(port):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), FakeDoor)
    cx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    cx.load_cert_chain(f"{ROOT}/keys/server.crt", f"{ROOT}/keys/server.key")
    srv.socket = cx.wrap_socket(srv.socket, server_side=True)
    print("ready", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--fake-door":
        fake_door(int(sys.argv[2]))
    sys.exit(main())
