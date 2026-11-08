#!/bin/sh
# Daily VictoriaMetrics data-dir size check -> ntfy warning.
# Fires ONCE per threshold crossing; re-arms when size drops back under
# (or when you raise THRESHOLD_BYTES after deciding what to do with the DB).
set -eu

DATA_DIR="${DATA_DIR:-/var/lib/victoriametrics}"
THRESHOLD_BYTES="${THRESHOLD_BYTES:-2147483648}"   # 2 GiB
STATE_FILE="${STATE_FILE:-/var/lib/edend/db-size-alerted}"
NTFY_URL="${NTFY_URL:-http://127.0.0.1:8090}"
NTFY_TOPIC="${NTFY_TOPIC:-eden_alerts}"
NTFY_CRED_FILE="${NTFY_CRED_FILE:-/etc/eden/ntfy.cred}"   # format: user:pass

size=$(du -sb "$DATA_DIR" | cut -f1)
human=$(du -sh "$DATA_DIR" | cut -f1)

if [ "$size" -ge "$THRESHOLD_BYTES" ]; then
    if [ ! -f "$STATE_FILE" ]; then
        curl -fsS -m 10 \
            -u "$(cat "$NTFY_CRED_FILE")" \
            -H "Title: Eden DB size threshold hit" \
            -H "Priority: high" \
            -H "Tags: floppy_disk,warning" \
            -d "VictoriaMetrics data dir is ${human} (threshold $((THRESHOLD_BYTES / 1024 / 1024)) MiB). Time to reconsider DB location." \
            "${NTFY_URL}/${NTFY_TOPIC}" >/dev/null
        touch "$STATE_FILE"
    fi
else
    rm -f "$STATE_FILE"
fi
