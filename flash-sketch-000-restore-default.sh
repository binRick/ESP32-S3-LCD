#!/usr/bin/env bash
# Restore the default flash image to the Waveshare ESP32-S3-Touch-LCD-1.69 board
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="/dev/cu.usbmodem1101"
IMAGE="$SCRIPT_DIR/backup/flash_backup_default.bin"

if [ ! -f "$IMAGE" ]; then
  echo "Error: Default image not found at $IMAGE"
  exit 1
fi

echo "Flashing default image to ESP32-S3 on $PORT ..."
"$SCRIPT_DIR/.venv/bin/esptool.py" \
  --port "$PORT" \
  --baud 460800 \
  write_flash \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 16MB \
  0x0 "$IMAGE"

echo "Done. Board has been restored to the default image."
