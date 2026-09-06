#!/usr/bin/env bash
# Startet den Geraeteersatz, faehrt die Verhaltenstests dagegen, raeumt auf.
# Haelt fest, dass der Mock sich wie die Firmware verhaelt (Routen, Pruefungen,
# Statuscodes, CSRF) -- ein laschererer Mock meldet gruen und luegt dabei.
set -euo pipefail
cd "$(dirname "$0")/.."

python3 tools/mock_api.py >/dev/null 2>&1 &
MOCK=$!
trap 'kill "$MOCK" 2>/dev/null || true; wait "$MOCK" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    if python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1',8077))==0 else 1)" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

python3 tools/test_mock.py
