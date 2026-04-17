#!/usr/bin/env bash
# flash-sketch-003-watch-face.sh
# Compile + flash the watch_face sketch to Waveshare ESP32-S3-Touch-LCD-1.69
#
# Usage:
#   ./flash-sketch-003-watch-face.sh            # compile + flash
#   ./flash-sketch-003-watch-face.sh --compile  # compile only
#   ./flash-sketch-003-watch-face.sh --flash    # flash last build only

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$SCRIPT_DIR/sketches/watch_face/watch_face.ino"
BUILD_DIR="$SCRIPT_DIR/build"
PORT="/dev/cu.usbmodem1101"
ESPTOOL="$SCRIPT_DIR/.venv/bin/esptool.py"
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[flash]${NC} $*"; }
warn()  { echo -e "${YELLOW}[flash]${NC} $*"; }
error() { echo -e "${RED}[flash]${NC} $*"; exit 1; }

DO_COMPILE=true
DO_FLASH=true
case "${1:-}" in
  --compile) DO_FLASH=false ;;
  --flash)   DO_COMPILE=false ;;
esac

# ── 1. arduino-cli ────────────────────────────────────────────────────────────
if ! command -v arduino-cli &>/dev/null; then
  brew install arduino-cli
fi
info "arduino-cli $(arduino-cli version | head -1)"

# ── 2. ESP32 core ─────────────────────────────────────────────────────────────
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32"; then
  arduino-cli config init --overwrite 2>/dev/null || true
  arduino-cli config add board_manager.additional_urls \
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  arduino-cli core update-index
  arduino-cli core install esp32:esp32
fi

# ── 3. Libraries ──────────────────────────────────────────────────────────────
ARDUINO_LIBS="$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Documents/Arduino")/libraries"

if [ ! -d "$ARDUINO_LIBS/Arduino_GFX" ]; then
  info "Installing Arduino_GFX..."
  curl -sL https://github.com/moononournation/Arduino_GFX/archive/refs/heads/master.zip -o /tmp/arduino_gfx.zip
  unzip -q /tmp/arduino_gfx.zip -d "$ARDUINO_LIBS"
  mv "$ARDUINO_LIBS/Arduino_GFX-master" "$ARDUINO_LIBS/Arduino_GFX"
  rm /tmp/arduino_gfx.zip
else
  info "Library already installed: Arduino_GFX"
fi

# ── 4. Compile ────────────────────────────────────────────────────────────────
if $DO_COMPILE; then
  info "Compiling: $SKETCH"
  mkdir -p "$BUILD_DIR"
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    --warnings default \
    "$SKETCH"
  info "Compile OK → $BUILD_DIR"
fi

# ── 5. Flash ──────────────────────────────────────────────────────────────────
if $DO_FLASH; then
  BIN="$BUILD_DIR/watch_face.ino.bin"
  BOOTLOADER="$BUILD_DIR/watch_face.ino.bootloader.bin"
  PARTITIONS="$BUILD_DIR/watch_face.ino.partitions.bin"
  BOOT_APP="$(find "$HOME/Library/Arduino15/packages/esp32" \
                   "$HOME/.arduino15/packages/esp32" \
              -name "boot_app0.bin" 2>/dev/null | head -1)"

  [ -f "$BIN" ]        || error "Binary not found — run --compile first"
  [ -f "$BOOTLOADER" ] || error "Bootloader not found"
  [ -f "$PARTITIONS" ] || error "Partitions not found"
  [ -f "$BOOT_APP" ]   || error "boot_app0.bin not found"

  info "Flashing to $PORT ..."
  "$ESPTOOL" \
    --chip esp32s3 \
    --port "$PORT" \
    --baud 460800 \
    --before default_reset \
    --after hard_reset \
    write_flash \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 16MB \
    0x0000  "$BOOTLOADER" \
    0x8000  "$PARTITIONS" \
    0xe000  "$BOOT_APP" \
    0x10000 "$BIN"

  info "Flash complete — board resetting."
fi
