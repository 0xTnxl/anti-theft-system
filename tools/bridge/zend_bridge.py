#!/usr/bin/env python3
"""Zend bridge: serves the door app at https://door.usezend.app with a trusted
(Let's Encrypt) certificate and relays every request to the interior board.

    phone --HTTPS (public cert)--> bridge --HTTPS (verified against keys/ca.crt)--> door 192.168.4.1

The bridge stores nothing and adds no logins of its own: the door still checks
the admin password, sessions, rate limits and request headers. It only:
  * terminates TLS with the trusted certificate,
  * forwards method, path, body and the headers the door needs,
  * keeps at most 2 connections open to the door (it has room for 3),
  * redirects http:// to https:// and adds HSTS.

    sudo python3 tools/bridge/zend_bridge.py --cert /etc/zend-bridge/fullchain.pem --key /etc/zend-bridge/key.pem
    python3 tools/bridge/zend_bridge.py --help

Only the Python standard library is used. See docs/BRIDGE.md.
"""
import argparse
import http.client
import http.server
import logging
import os
import queue
import socket
import ssl
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HOP = {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "te", "trailer", "upgrade",
       "proxy-authorization", "proxy-authenticate", "host"}
FORWARD_REQ = {"cookie", "content-type", "content-length", "x-atds", "accept", "accept-language", "user-agent"}
MAX_BODY = 2 * 1024 * 1024          # OTA packages are ~1.2 MB
log = logging.getLogger("zend-bridge")


class DoorConnection(http.client.HTTPSConnection):
    """HTTPS to the door, verifying its certificate for `server_name` (its own
    address, 192.168.4.1) even when we dial something else (tests, tunnels)."""

    def __init__(self, host, port, server_name, ctx, timeout):
        super().__init__(host, port, timeout=timeout, context=ctx)
        self.server_name, self._ctx = server_name, ctx

    def connect(self):
        sock = socket.create_connection((self.host, self.port), self.timeout)
        self.sock = self._ctx.wrap_socket(sock, server_hostname=self.server_name)


class DoorPool:
    """Small pool of persistent HTTPS connections to the door."""

    def __init__(self, host, port, server_name, cafile, size, timeout):
        self.host, self.port, self.timeout = host, port, timeout
        self.ctx = ssl.create_default_context(cafile=cafile)
        self.ctx.check_hostname = True
        self.server_name = server_name
        self.free = queue.LifoQueue()
        self.slots = threading.BoundedSemaphore(size)

    def _new(self):
        return DoorConnection(self.host, self.port, self.server_name, self.ctx, self.timeout)

    def request(self, method, path, body, headers, timeout):
        if not self.slots.acquire(timeout=timeout):
            raise TimeoutError("door busy")
        try:
            for attempt in (1, 2):
                try:
                    conn = self.free.get_nowait()
                except queue.Empty:
                    conn = self._new()
                fresh = conn.sock is None
                try:
                    conn.timeout = timeout
                    if conn.sock:
                        conn.sock.settimeout(timeout)
                    conn.request(method, path, body=body, headers=headers)
                    resp = conn.getresponse()
                    data = resp.read()
                    if resp.will_close:
                        conn.close()
                    else:
                        self.free.put(conn)
                    return resp.status, resp.reason, resp.getheaders(), data
                except (http.client.RemoteDisconnected, ConnectionResetError, BrokenPipeError,
                        http.client.CannotSendRequest, http.client.BadStatusLine):
                    conn.close()
                    if fresh or attempt == 2 or method not in ("GET", "HEAD"):
                        raise
                    # A kept-alive socket the door already closed: retry once on a new one.
                except Exception:
                    conn.close()
                    raise
        finally:
            self.slots.release()


def make_handler(pool, args):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "zend-bridge"
        sys_version = ""

        def log_message(self, fmt, *a):
            log.info("%s %s", self.client_address[0], fmt % a)

        def _send(self, code, reason, headers, body):
            self.send_response(code, reason)
            for k, v in headers:
                if k.lower() not in HOP and k.lower() != "content-length":
                    self.send_header(k, v)
            self.send_header("Strict-Transport-Security", "max-age=31536000")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _error(self, code, msg):
            body = ('{"ok":false,"msg":"%s"}' % msg).encode()
            self._send(code, None, [("Content-Type", "application/json"), ("Cache-Control", "no-store")], body)

        def _relay(self):
            if self.path == "/bridge/health":
                return self._send(200, None, [("Content-Type", "text/plain")], b"ok\n")
            n = int(self.headers.get("Content-Length") or 0)
            if n > MAX_BODY:
                return self._error(413, "too large")
            body = self.rfile.read(n) if n else None
            hdr = {k: v for k, v in self.headers.items() if k.lower() in FORWARD_REQ}
            hdr["Host"] = args.door_name
            hdr["X-Forwarded-For"] = self.client_address[0]
            timeout = args.ota_timeout if self.path.startswith("/api/ota") else args.timeout
            try:
                code, reason, headers, data = pool.request(self.command, self.path, body, hdr, timeout)
            except (OSError, ssl.SSLError, http.client.HTTPException, TimeoutError) as e:
                log.warning("door unreachable for %s %s: %s", self.command, self.path, e)
                return self._error(502, "the door is not answering")
            self._send(code, reason, headers, data)

        do_GET = do_POST = do_HEAD = _relay

    return Handler


class Redirect(http.server.BaseHTTPRequestHandler):
    host = "door.usezend.app"

    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(301)
        self.send_header("Location", f"https://{self.host}{self.path}")
        self.send_header("Content-Length", "0")
        self.end_headers()

    do_HEAD = do_POST = do_GET


def serve(server, name):
    t = threading.Thread(target=server.serve_forever, name=name, daemon=True)
    t.start()
    return t


def main():
    ap = argparse.ArgumentParser(description="Zend bridge: trusted HTTPS for the door app")
    ap.add_argument("--host", default="door.usezend.app", help="public name of the app")
    ap.add_argument("--cert", required=True, help="certificate chain (PEM), e.g. Let's Encrypt fullchain")
    ap.add_argument("--key", required=True, help="private key (PEM)")
    ap.add_argument("--listen", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--http-port", type=int, default=80, help="0 = no http->https redirect")
    ap.add_argument("--door", default="192.168.4.1", help="address the bridge dials")
    ap.add_argument("--door-port", type=int, default=443)
    ap.add_argument("--door-name", default="192.168.4.1", help="name on the door's certificate")
    ap.add_argument("--door-ca", default=os.path.join(ROOT, "keys", "ca.crt"))
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--ota-timeout", type=float, default=200)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING,
                        format="%(asctime)s %(levelname)s %(message)s")

    pool = DoorPool(args.door, args.door_port, args.door_name, args.door_ca, size=2, timeout=args.timeout)
    tls = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    tls.minimum_version = ssl.TLSVersion.TLSv1_2
    tls.load_cert_chain(args.cert, args.key)
    httpd = http.server.ThreadingHTTPServer((args.listen, args.port), make_handler(pool, args))
    httpd.daemon_threads = True
    httpd.socket = tls.wrap_socket(httpd.socket, server_side=True)
    serve(httpd, "https")
    if args.http_port:
        Redirect.host = args.host
        r = http.server.ThreadingHTTPServer((args.listen, args.http_port), Redirect)
        r.daemon_threads = True
        serve(r, "http")
    print(f"zend-bridge: https://{args.host} (port {args.port}) -> https://{args.door}:{args.door_port}", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
