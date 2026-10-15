#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:twatchs3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,Revision=Radio_SX1262"
OUTPUT_DIR="$PROJECT_DIR/build/esp32.esp32.twatchs3"

CLI=$(which arduino-cli 2>/dev/null || true)
if [ -z "$CLI" ]; then
    CANDIDATES=(
        "$HOME/.local/bin/arduino-cli"
        "$HOME/bin/arduino-cli"
        "/var/lib/flatpak/app/cc.arduino.IDE2/x86_64/stable/active/files/arduino-ide/resources/app/lib/backend/resources/arduino-cli"
    )
    for CAND in "${CANDIDATES[@]}"; do
        if [ -x "$CAND" ]; then
            CLI="$CAND"
            break
        fi
    done
fi

if [ -z "$CLI" ]; then
    echo "[FEHLER] arduino-cli nicht im PATH oder in Flatpak-Installationen gefunden!"
    echo "Bitte arduino-cli installieren (z. B. via: curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh)"
    exit 2
fi

CLEAN_ARG=""
if [ "$1" == "--clean" ]; then
    CLEAN_ARG="--clean"
    echo "[BUILD] Vollstaendiger Rebuild (clean) angefordert."
fi

BUILD_PATH="$PROJECT_DIR/build/obj"
CACHE_PATH="$PROJECT_DIR/build/cache"
mkdir -p "$OUTPUT_DIR" "$BUILD_PATH" "$CACHE_PATH"

N_JOBS=$(nproc 2>/dev/null || echo 4)

echo "[BUILD] Kompiliere WatchOS Firmware mit $CLI ($N_JOBS Jobs, Cache aktiviert)..."
echo "[BOARD] $FQBN"

START_TIME=$(date +%s)
"$CLI" compile $CLEAN_ARG \
    --jobs "$N_JOBS" \
    --build-path "$BUILD_PATH" \
    --fqbn "$FQBN" \
    --output-dir "$OUTPUT_DIR" \
    "$PROJECT_DIR"
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

BIN_FILE="$OUTPUT_DIR/WatchOS.ino.bin"
if [ -f "$BIN_FILE" ]; then
    SIZE=$(stat -c%s "$BIN_FILE" 2>/dev/null || stat -f%z "$BIN_FILE" 2>/dev/null || echo "unbekannt")
    echo "[OK] Firmware erfolgreich gebaut in ${DURATION}s"
    echo "[BIN] $BIN_FILE ($SIZE Bytes)"
else
    echo "[FEHLER] Kompilierung abgeschlossen, aber $BIN_FILE nicht gefunden!"
    exit 3
fi

