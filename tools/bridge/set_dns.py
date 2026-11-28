#!/usr/bin/env python3
"""Point door.usezend.app at the bridge's address on the home network (name.com API v4).

    export NAMECOM_USER=<name.com username>  NAMECOM_TOKEN=<API token>
    python3 tools/bridge/set_dns.py                 # uses this PC's address on the default route
    python3 tools/bridge/set_dns.py --ip 192.168.1.50
    python3 tools/bridge/set_dns.py --show

Creates or updates one A record (TTL 300). The address is private (home
network only), which is intended: phones reach it at home, and from away
through Tailscale. Get an API token at name.com > Account > Security > API.
"""
import argparse
import base64
import ipaddress
import json
import os
import socket
import sys
import urllib.error
import urllib.request

API = "https://api.name.com/v4/domains/{domain}/records"


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("1.1.1.1", 53))
        return s.getsockname()[0]
    finally:
        s.close()


def call(method, url, body=None):
    user, token = os.environ.get("NAMECOM_USER"), os.environ.get("NAMECOM_TOKEN")
    if not user or not token:
        sys.exit("set NAMECOM_USER and NAMECOM_TOKEN first")
    req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body else None,
                                 headers={"Authorization": "Basic " + base64.b64encode(f"{user}:{token}".encode()).decode(),
                                          "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        sys.exit(f"name.com API error {e.code}: {e.read().decode(errors='replace')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--domain", default="usezend.app")
    ap.add_argument("--name", default="door", help="host part (door -> door.usezend.app)")
    ap.add_argument("--ip", help="address to publish (default: this PC's LAN address)")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()
    url = API.format(domain=args.domain)
    recs = [r for r in call("GET", url).get("records", []) if r.get("host") == args.name and r.get("type") == "A"]
    if args.show:
        print("\n".join(f"{r['fqdn']} A {r['answer']} (ttl {r.get('ttl')})" for r in recs) or "no A record")
        return 0
    ip = args.ip or local_ip()
    ipaddress.ip_address(ip)
    if not ipaddress.ip_address(ip).is_private:
        print(f"warning: {ip} is a public address; the bridge is meant to stay on the home network")
    body = {"host": args.name, "type": "A", "answer": ip, "ttl": 300}
    if recs:
        call("PUT", f"{url}/{recs[0]['id']}", body)
        print(f"updated {args.name}.{args.domain} -> {ip}")
    else:
        call("POST", url, body)
        print(f"created {args.name}.{args.domain} -> {ip}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
