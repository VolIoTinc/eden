#!/bin/bash
# Eden Pi provisioning — idempotent, safe to re-run.
# Installs: mosquitto, VictoriaMetrics, Grafana, ntfy, edend, db-size timer.
# Deliberately does NOT touch: iptables, dnsmasq, NetworkManager, Tailscale (Serenity).
set -euo pipefail

VM_VERSION="${VM_VERSION:-1.115.0}"
NTFY_VERSION="${NTFY_VERSION:-2.14.0}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=arm64
# Grafana and ntfy bind to the Tailscale IP only: the Pi has a public LTE interface and
# we don't touch iptables (Serenity owns it), so binding is the firewall.
TS_IP="${TS_IP:-$(tailscale ip -4 2>/dev/null || true)}"
[ -n "$TS_IP" ] || { echo "cannot determine Tailscale IP (tailscale ip -4); set TS_IP=..."; exit 1; }

[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }

say() { printf '\n\033[1;32m== %s\033[0m\n' "$*"; }

ensure_user() {
    id "$1" >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin "$1"
}

say "apt packages"
apt-get update -qq
apt-get install -y -qq mosquitto mosquitto-clients curl python3-venv python3-pip apt-transport-https software-properties-common

say "users + directories"
ensure_user victoriametrics
ensure_user ntfy
ensure_user edend
install -d -o victoriametrics -g victoriametrics /var/lib/victoriametrics
install -d -o ntfy -g ntfy /var/cache/ntfy /var/lib/ntfy /etc/ntfy
install -d -o edend -g edend /var/lib/edend /var/lib/edend/spool
install -d /etc/eden /opt/eden

say "mosquitto"
install -m 644 "$REPO_DIR/pi/mosquitto/eden.conf" /etc/mosquitto/conf.d/eden.conf
install -m 640 -g mosquitto "$REPO_DIR/pi/mosquitto/eden.acl" /etc/mosquitto/eden.acl
if [ ! -f /etc/mosquitto/eden.passwd ]; then
    touch /etc/mosquitto/eden.passwd
    chmod 640 /etc/mosquitto/eden.passwd
    chgrp mosquitto /etc/mosquitto/eden.passwd
    NEED_MQTT_PASSWORDS=1
fi
systemctl enable mosquitto >/dev/null

say "VictoriaMetrics ${VM_VERSION}"
if [ ! -x /opt/victoriametrics/victoria-metrics-prod ] || \
   ! /opt/victoriametrics/victoria-metrics-prod --version 2>/dev/null | grep -q "$VM_VERSION"; then
    install -d /opt/victoriametrics
    curl -fsSL -o /tmp/vm.tar.gz \
        "https://github.com/VictoriaMetrics/VictoriaMetrics/releases/download/v${VM_VERSION}/victoria-metrics-linux-${ARCH}-v${VM_VERSION}.tar.gz"
    tar -xzf /tmp/vm.tar.gz -C /opt/victoriametrics
    rm /tmp/vm.tar.gz
fi
install -m 644 "$REPO_DIR/pi/victoriametrics/victoriametrics.service" /etc/systemd/system/victoriametrics.service

say "ntfy ${NTFY_VERSION}"
if [ ! -x /opt/ntfy/ntfy ] || ! /opt/ntfy/ntfy --version 2>/dev/null | grep -q "$NTFY_VERSION"; then
    install -d /opt/ntfy
    curl -fsSL -o /tmp/ntfy.tar.gz \
        "https://github.com/binwiederhier/ntfy/releases/download/v${NTFY_VERSION}/ntfy_${NTFY_VERSION}_linux_${ARCH}.tar.gz"
    tar -xzf /tmp/ntfy.tar.gz -C /tmp
    install -m 755 "/tmp/ntfy_${NTFY_VERSION}_linux_${ARCH}/ntfy" /opt/ntfy/ntfy
    rm -rf /tmp/ntfy.tar.gz "/tmp/ntfy_${NTFY_VERSION}_linux_${ARCH}"
fi
sed "s/__TS_IP__/$TS_IP/g" "$REPO_DIR/pi/ntfy/server.yml" > /etc/ntfy/server.yml && chmod 644 /etc/ntfy/server.yml
install -m 644 "$REPO_DIR/pi/ntfy/ntfy.service" /etc/systemd/system/ntfy.service

say "grafana"
if ! dpkg -s grafana >/dev/null 2>&1; then
    mkdir -p /etc/apt/keyrings
    curl -fsSL https://apt.grafana.com/gpg.key | gpg --dearmor > /etc/apt/keyrings/grafana.gpg
    echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" \
        > /etc/apt/sources.list.d/grafana.list
    apt-get update -qq
    apt-get install -y -qq grafana
fi
install -m 644 -o grafana -g grafana \
    "$REPO_DIR/pi/grafana/provisioning/datasources/victoriametrics.yaml" \
    /etc/grafana/provisioning/datasources/victoriametrics.yaml
install -m 644 -o grafana -g grafana \
    "$REPO_DIR/pi/grafana/provisioning/dashboards/eden.yaml" \
    /etc/grafana/provisioning/dashboards/eden.yaml
install -d -o grafana -g grafana /var/lib/grafana/dashboards/eden
install -m 644 -o grafana -g grafana \
    "$REPO_DIR/pi/grafana/dashboards/eden-overview.json" \
    /var/lib/grafana/dashboards/eden/eden-overview.json
install -d /etc/systemd/system/grafana-server.service.d
sed "s/__TS_IP__/$TS_IP/g" "$REPO_DIR/pi/grafana/grafana-server.override.conf" \
    > /etc/systemd/system/grafana-server.service.d/eden.conf
systemctl enable grafana-server >/dev/null

say "edend"
if [ ! -d "$REPO_DIR/edend" ]; then
    echo "  (edend/ not in repo yet — skipping; re-run install.sh once it exists)"
    SKIP_EDEND=1
fi
if [ -z "${SKIP_EDEND:-}" ]; then
if [ ! -d /opt/edend/venv ]; then
    python3 -m venv /opt/edend/venv
fi
/opt/edend/venv/bin/pip install -q --upgrade pip
/opt/edend/venv/bin/pip install -q "$REPO_DIR/edend"
install -m 644 "$REPO_DIR/pi/edend/edend.service" /etc/systemd/system/edend.service
if [ ! -f /etc/eden/edend.yaml ]; then
    install -m 640 -o edend -g edend "$REPO_DIR/pi/edend/edend.yaml.example" /etc/eden/edend.yaml
    NEED_EDEND_CONFIG=1
fi
if [ ! -f /etc/eden/plants.yaml ]; then
    install -m 640 -o edend -g edend "$REPO_DIR/edend/edend/plants.yaml.example" /etc/eden/plants.yaml
fi
fi  # SKIP_EDEND

say "db-size check"
install -m 755 "$REPO_DIR/pi/db-size-check/check-db-size.sh" /opt/eden/check-db-size.sh
install -m 644 "$REPO_DIR/pi/db-size-check/db-size-check.service" /etc/systemd/system/db-size-check.service
install -m 644 "$REPO_DIR/pi/db-size-check/db-size-check.timer" /etc/systemd/system/db-size-check.timer

say "starting services"
systemctl daemon-reload
systemctl enable --now victoriametrics ntfy db-size-check.timer >/dev/null
systemctl restart mosquitto grafana-server

echo
echo "======================================================================"
echo "Install complete. One-time manual steps (only if not done already):"
echo
if [ "${NEED_MQTT_PASSWORDS:-0}" = 1 ]; then
    echo "  # MQTT users:"
    echo "  mosquitto_passwd /etc/mosquitto/eden.passwd eden-node"
    echo "  mosquitto_passwd /etc/mosquitto/eden.passwd edend"
    echo "  systemctl restart mosquitto"
    echo
fi
echo "  # ntfy users + ACLs (skip if 'ntfy user list' already shows them):"
echo "  NTFY_AUTH_FILE=/var/lib/ntfy/user.db /opt/ntfy/ntfy user add --role=user eden"
echo "  NTFY_AUTH_FILE=/var/lib/ntfy/user.db /opt/ntfy/ntfy user add --role=user andy"
echo "  NTFY_AUTH_FILE=/var/lib/ntfy/user.db /opt/ntfy/ntfy access eden 'eden_*' write-only"
echo "  NTFY_AUTH_FILE=/var/lib/ntfy/user.db /opt/ntfy/ntfy access andy 'eden_*' read-only"
echo "  echo 'eden:<password>' > /etc/eden/ntfy.cred && chmod 600 /etc/eden/ntfy.cred && chown edend /etc/eden/ntfy.cred"
echo
if [ "${NEED_EDEND_CONFIG:-0}" = 1 ]; then
    echo "  # Edit credentials + plants, then start edend:"
    echo "  nano /etc/eden/edend.yaml /etc/eden/plants.yaml"
    echo "  systemctl enable --now edend"
    echo
fi
echo "  # Grafana: http://$TS_IP:3000  (admin/admin on first login — change it)"
echo "  # Phone: ntfy app -> add server http://$TS_IP:8090, user andy,"
echo "  #        subscribe to topic 'eden_alerts'"
echo
echo "  # Smoke tests:"
echo "  mosquitto_sub -h 127.0.0.1 -u edend -P <pw> -t 'eden/#' -v   # in one shell"
echo "  mosquitto_pub -h 127.0.0.1 -u edend -P <pw> -t eden/test -m hello"
echo "  curl -d 'ping from Eden' -u eden:<pw> http://127.0.0.1:8090/eden_alerts"
echo "  curl 'http://127.0.0.1:8428/write' --data-binary 'test,src=install v=1'"
echo "======================================================================"
