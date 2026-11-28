#!/usr/bin/env bash
# One-time setup of the Zend bridge on a Linux PC or Raspberry Pi.
#
#   export NAMECOM_USER=<name.com username> NAMECOM_TOKEN=<name.com API token>
#   export ACME_EMAIL=<your email for expiry notices>
#   sudo -E bash tools/bridge/setup_bridge.sh
#
# What it does (see docs/BRIDGE.md):
#   1. installs acme.sh and gets a Let's Encrypt certificate for door.usezend.app
#      with the DNS-01 challenge through the name.com API (no open ports needed)
#   2. installs the certificate into /etc/zend-bridge, renews it automatically
#      (acme.sh cron) and restarts the bridge after each renewal
#   3. installs and starts the zend-bridge systemd service
# Joining the ATDS-Door WiFi and the DNS record are separate steps (docs/BRIDGE.md).
set -euo pipefail
HOST="${ZEND_HOST:-door.usezend.app}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CERTDIR=/etc/zend-bridge
: "${NAMECOM_USER:?set NAMECOM_USER}" "${NAMECOM_TOKEN:?set NAMECOM_TOKEN}" "${ACME_EMAIL:?set ACME_EMAIL}"
[ "$(id -u)" = 0 ] || { echo "run with sudo -E"; exit 1; }
[ -f "$REPO/keys/ca.crt" ] || { echo "keys/ca.crt missing: run python3 tools/gen_device_keys.py first"; exit 1; }

# sudo -E keeps the caller's HOME, which made acme.sh install into the user's
# home as root. Pin everything to root's home.
export HOME=/root
ACME_HOME=/root/.acme.sh
ACME="$ACME_HOME/acme.sh"
if [ ! -x "$ACME" ]; then
  echo "== installing acme.sh"
  curl -fsSL https://raw.githubusercontent.com/acmesh-official/acme.sh/master/acme.sh \
    | sh -s -- --install-online --home "$ACME_HOME" -m "$ACME_EMAIL"
fi
export Namecom_Username="$NAMECOM_USER" Namecom_Token="$NAMECOM_TOKEN"

echo "== requesting a Let's Encrypt certificate for $HOST (DNS-01 via name.com)"
"$ACME" --set-default-ca --server letsencrypt
"$ACME" --issue --dns dns_namecom -d "$HOST" --keylength ec-256 || [ $? = 2 ]   # 2 = still valid, skipped

install -d -m 750 "$CERTDIR"
install -d -m 755 "$CERTDIR/door"
install -m 644 "$REPO/keys/ca.crt" "$CERTDIR/door/ca.crt"
"$ACME" --install-cert -d "$HOST" --ecc \
  --fullchain-file "$CERTDIR/fullchain.pem" --key-file "$CERTDIR/key.pem" \
  --reloadcmd "systemctl restart zend-bridge || true"
chmod 640 "$CERTDIR/key.pem"

echo "== installing the bridge service"
install -d /opt/zend-bridge
install -m 755 "$REPO/tools/bridge/zend_bridge.py" /opt/zend-bridge/zend_bridge.py
id zend-bridge >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin zend-bridge
chgrp -R zend-bridge "$CERTDIR"
sed "s#@HOST@#$HOST#g" "$REPO/tools/bridge/zend-bridge.service" > /etc/systemd/system/zend-bridge.service
systemctl daemon-reload
systemctl enable --now zend-bridge
sleep 2
systemctl --no-pager --lines 5 status zend-bridge || true
echo "== done: https://$HOST  (check: curl https://$HOST/bridge/health)"
