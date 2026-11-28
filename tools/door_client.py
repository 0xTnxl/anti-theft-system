"""HTTPS client for the door's app API, shared by the PC tools.

The PC talks to the door at 192.168.4.1 (on the ATDS-Door WiFi). The door
presents either its Let's Encrypt certificate for door.usezend.app, or, before
one is installed, the local-CA certificate for 192.168.4.1 (keys/ca.crt).
Both are checked properly: the name in the certificate must match.
"""
import http.client
import json
import os
import socket
import ssl
import urllib.parse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HOST = "door.usezend.app"


class DoorError(Exception):
    pass


class _Conn(http.client.HTTPSConnection):
    """Connects to `ip` but checks the certificate for `name`."""

    def __init__(self, ip, name, ctx, timeout):
        super().__init__(ip, 443, timeout=timeout, context=ctx)
        self._name = name

    def connect(self):
        sock = socket.create_connection((self.host, self.port), self.timeout)
        self.sock = self._context.wrap_socket(sock, server_hostname=self._name)


class Door:
    def __init__(self, ip="192.168.4.1", timeout=30):
        self.ip, self.timeout = ip, timeout
        self.ctx = ssl.create_default_context()
        ca = os.path.join(ROOT, "keys", "ca.crt")
        if os.path.exists(ca):
            self.ctx.load_verify_locations(ca)
        self.name = None
        self.cookie = ""

    def _pick_name(self):
        errors = []
        for name in (HOST, self.ip):
            try:
                c = _Conn(self.ip, name, self.ctx, 10)
                c.connect()
                c.close()
                return name
            except ssl.SSLCertVerificationError as e:
                errors.append(f"{name}: {e.verify_message}")
            except OSError as e:
                raise DoorError(f"cannot reach the door at {self.ip}: {e}. Is this PC on the ATDS-Door WiFi?")
        raise DoorError("the door's certificate did not check out (" + "; ".join(errors) + ")")

    def call(self, method, path, body=b"", ctype="application/x-www-form-urlencoded", timeout=None):
        if not self.name:
            self.name = self._pick_name()
        c = _Conn(self.ip, self.name, self.ctx, timeout or self.timeout)
        hdr = {"X-ATDS": "1", "Content-Type": ctype, "Host": self.name}
        if self.cookie:
            hdr["Cookie"] = self.cookie
        c.request(method, path, body=body or None, headers=hdr)
        r = c.getresponse()
        for k, v in r.getheaders():
            if k.lower() == "set-cookie" and v.startswith("atds="):
                self.cookie = v.split(";", 1)[0]
        data = r.read()
        c.close()
        try:
            return json.loads(data or b"{}")
        except ValueError:
            raise DoorError(f"unexpected answer from the door (HTTP {r.status})")

    def login(self, password):
        r = self.call("POST", "/api/login", urllib.parse.urlencode({"password": password}).encode())
        if not r.get("ok"):
            raise DoorError(f"login failed: {r.get('msg')}")
