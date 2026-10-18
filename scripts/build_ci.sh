#!/usr/bin/env bash
# ==============================================================================
# WatchOS CI/CD Build & Release Artifacts Generator
# Builds firmware for LilyGO T-Watch S3 and produces sha256 checksums + version.json
# ==============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

OUTPUT_DIR="$PROJECT_DIR/build_output"
mkdir -p "$OUTPUT_DIR"

echo "=== WatchOS Automated Build Script ==="
echo "Project Directory: $PROJECT_DIR"

# 1. Compile Firmware via PlatformIO
echo "[BUILD] Compiling WatchOS with PlatformIO (env: twatch-s3)..."
pio run -e twatch-s3

BIN_SRC="$PROJECT_DIR/.pio/build/twatch-s3/firmware.bin"
if [ ! -f "$BIN_SRC" ]; then
    echo "[ERROR] firmware.bin was not generated at: $BIN_SRC"
    exit 1
fi

BIN_SIZE=$(stat -c%s "$BIN_SRC" 2>/dev/null || stat -f%z "$BIN_SRC" 2>/dev/null || wc -c < "$BIN_SRC")
echo "[OK] Firmware compiled successfully ($BIN_SIZE bytes)"

# 2. Stage Artifacts for GitHub Release and OTA
cp "$BIN_SRC" "$OUTPUT_DIR/firmware.bin"
cp "$BIN_SRC" "$OUTPUT_DIR/WatchOS-firmware.bin"

cd "$OUTPUT_DIR"

# Compute Checksums
sha256sum firmware.bin > firmware.bin.sha256
sha256sum WatchOS-firmware.bin > WatchOS-firmware.bin.sha256
SHA256_VAL=$(cut -d' ' -f1 firmware.bin.sha256)

# Extract Version Metadata
VER=$(grep -E 'WATCHOS_VERSION' "$PROJECT_DIR/Globals.h" | head -1 | sed -E 's/.*"([^"]+)".*/\1/' || echo "2.4.0")
COMMIT_HASH="${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo "unknown")}"
COMMIT_SHORT=$(echo "$COMMIT_HASH" | cut -c1-7)
BUILD_DATE=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat <<EOF > version.json
{
  "version": "$VER",
  "commit": "$COMMIT_SHORT",
  "commit_full": "$COMMIT_HASH",
  "build_date": "$BUILD_DATE",
  "target": "LilyGO T-Watch S3 (ESP32-S3)",
  "bin_file": "firmware.bin",
  "bin_size_bytes": $BIN_SIZE,
  "sha256": "$SHA256_VAL",
  "release_url": "https://github.com/diggaknoxville/WatchOS/releases/tag/latest"
}
EOF

# Copy version.json to root so repository root can also serve as a reference
cp version.json "$PROJECT_DIR/version.json"

echo "=== Build Output Generated ==="
ls -lh "$OUTPUT_DIR"
cat "$OUTPUT_DIR/version.json"
echo "=== Done ==="

