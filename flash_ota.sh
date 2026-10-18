#!/usr/bin/env bash
set -e

WATCH_IP="${1:-192.168.4.1}"
BIN_FILE="build/esp32.esp32.twatchs3/WatchOS.ino.bin"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$PROJECT_DIR"

# 1. Prüfen ob Quellcode neuer als Binary ist -> ggf. automatisch bauen
NEED_BUILD=0
if [ ! -f "$BIN_FILE" ]; then
    NEED_BUILD=1
else
    for SRC in $(find . -maxdepth 1 \( -name "*.cpp" -o -name "*.h" -o -name "*.ino" \)); do
        if [ "$SRC" -nt "$BIN_FILE" ]; then
            NEED_BUILD=1
            break
        fi
    done
fi

if [ "$NEED_BUILD" -eq 1 ]; then
    echo "[BUILD] Quellcode geändert, kompiliere neu..."
    ./build_firmware.sh
fi

if [ ! -f "$BIN_FILE" ]; then
    echo "[FEHLER] Binary $BIN_FILE nicht gefunden!"
    exit 1
fi

SIZE=$(stat -c%s "$BIN_FILE" 2>/dev/null || stat -f%z "$BIN_FILE" 2>/dev/null)
echo "[INFO] Firmware: $BIN_FILE ($SIZE Bytes)"
echo "[CONNECT] Prüfe Verbindung zu http://$WATCH_IP/ (warte bis zu 60s auf Wachzustand)..."
CONNECTED=0
for i in $(seq 1 60); do
    if curl -s -m 5 "http://$WATCH_IP/api/status" > /dev/null; then
        CONNECTED=1
        echo "[CONNECT] WatchOS erreicht nach ${i}s!"
        break
    fi
    sleep 1
done

if [ "$CONNECTED" -eq 0 ]; then
    echo "[FEHLER] WatchOS unter http://$WATCH_IP/ nicht erreichbar!"
    echo "Bitte sicherstellen, dass die Uhr an ist (Display berühren / am Ladegerät) oder mit dem Hotspot ('WatchOS' / 192.168.4.1) verbunden ist."
    exit 2
fi

echo "[OTA] Starte Firmware-Upload zu http://$WATCH_IP/update?size=$SIZE ..."
rm -f /tmp/ota_response.txt
HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/ota_response.txt --max-time 180 -H "Expect:" -H "Connection: close" -F "update=@$BIN_FILE" "http://$WATCH_IP/update?size=$SIZE" || echo "000")
RESPONSE=$(cat /tmp/ota_response.txt 2>/dev/null || echo "FAIL")

echo ""
if [ "$RESPONSE" = "OK" ]; then
    echo "[OK] Update erfolgreich übertragen! Die Uhr startet jetzt neu..."
    echo "[WAIT] Warte auf Neustart..."
    sleep 7
    HOST_EPOCH=$(date +%s)
    echo "[TIME] Synchronisiere aktuelle Uhrzeit ($HOST_EPOCH) mit der Watch..."
    curl -s -m 4 -X POST "http://$WATCH_IP/api/sync_time?epoch=$HOST_EPOCH" > /dev/null 2>&1 || true
    echo "[READY] WatchOS läuft jetzt mit der neuen Firmware und synchronisierter Uhrzeit."
else
    echo "[ANTWORT] $RESPONSE"
    if [ "$RESPONSE" = "FAIL" ]; then
        echo "[FEHLER] Die Uhr meldet Fehler beim Flashen."
        exit 3
    fi
fi

