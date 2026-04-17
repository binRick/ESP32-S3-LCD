#!/usr/bin/env bash
# flash-sketch-001-cube-3d.sh
# Compile + flash the cube_3d sketch to Waveshare ESP32-S3-Touch-LCD-1.69
#
# Usage:
#   ./flash-sketch-001-cube-3d.sh            # compile + flash
#   ./flash-sketch-001-cube-3d.sh --compile  # compile only
#   ./flash-sketch-001-cube-3d.sh --flash    # flash last build only

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$SCRIPT_DIR/sketches/cube_3d/cube_3d.ino"
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
  info "Installing arduino-cli via Homebrew..."
  command -v brew &>/dev/null || error "Homebrew not found — install from https://brew.sh"
  brew install arduino-cli
fi
info "arduino-cli $(arduino-cli version | head -1)"

# ── 2. ESP32 core ─────────────────────────────────────────────────────────────
if ! arduino-cli core list 2>/dev/null | grep -q "esp32:esp32"; then
  info "Adding Espressif board index..."
  arduino-cli config init --overwrite 2>/dev/null || true
  arduino-cli config add board_manager.additional_urls \
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  arduino-cli core update-index
  info "Installing esp32:esp32 core..."
  arduino-cli core install esp32:esp32
else
  info "Core esp32:esp32 already installed"
fi

# ── 3. Libraries ──────────────────────────────────────────────────────────────
ARDUINO_LIBS="$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Documents/Arduino")/libraries"

if ! arduino-cli lib list 2>/dev/null | grep -q "^TFT_eSPI"; then
  info "Installing TFT_eSPI..."
  arduino-cli lib install "TFT_eSPI"
else
  info "Library already installed: TFT_eSPI"
fi

# Patch TFT_eSPI User_Setup.h for Waveshare ESP32-S3-Touch-LCD-1.69
TFTESPI_DIR="$ARDUINO_LIBS/TFT_eSPI"
if [ -d "$TFTESPI_DIR" ]; then
  info "Patching TFT_eSPI User_Setup.h for Waveshare ESP32-S3-Touch-LCD-1.69..."
  cp "$SCRIPT_DIR/sketches/cube_3d/User_Setup.h" "$TFTESPI_DIR/User_Setup.h"
else
  warn "TFT_eSPI dir not found at $TFTESPI_DIR — skipping patch"
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
  BIN="$BUILD_DIR/cube_3d.ino.bin"
  BOOTLOADER="$BUILD_DIR/cube_3d.ino.bootloader.bin"
  PARTITIONS="$BUILD_DIR/cube_3d.ino.partitions.bin"
  BOOT_APP="$(find "$HOME/Library/Arduino15/packages/esp32" \
                   "$HOME/.arduino15/packages/esp32" \
              -name "boot_app0.bin" 2>/dev/null | head -1)"

  [ -f "$BIN" ]        || error "Binary not found — run --compile first"
  [ -f "$BOOTLOADER" ] || error "Bootloader not found"
  [ -f "$PARTITIONS" ] || error "Partitions not found"
  [ -f "$BOOT_APP" ]   || error "boot_app0.bin not found in Arduino ESP32 core"

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
