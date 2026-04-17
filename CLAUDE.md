# ESP32-S3-Touch-LCD-1.69 Development Project

## Hardware

**Board:** Waveshare ESP32-S3-Touch-LCD-1.69
- ESP32-S3 dual-core 32-bit 240 MHz LX7 microcontroller
- 16MB Flash, 8MB OPI PSRAM
- WiFi + Bluetooth 5 (LE)

### LCD Display
- **Driver:** ST7789V2, 1.69 inch, 240×280
- **Interface:** SPI
- SCLK=GPIO18, CS=GPIO16, RST=GPIO3, DC=GPIO2, BL=GPIO17
- Colors: 262K (RGB565)

### Touch Controller
- **Driver:** CST816T (capacitive I2C)
- SDA=GPIO11, SCL=GPIO10

### IMU
- **Driver:** QMI8658 (6-axis: accelerometer + gyroscope)
- SDA=GPIO11, SCL=GPIO10

### RTC
- **Driver:** PCF85063
- SDA=GPIO11, SCL=GPIO10
- Reserved SH1.0 battery header onboard

### Power
- USB-C (power + programming, native USB CDC)
- MX1.25 lithium battery connector
- Charging IC: ETA6098

## Serial Port

**Port:** `/dev/cu.usbmodem1101`

Native USB CDC — no external USB-UART bridge needed.

### arduino-cli FQBN

```
esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi
```

## Reference Project

`esp32-dev-001/` (git submodule) — previous project on a Freenove ESP32-S3 board using the same arduino-cli/esptool.py workflow. Reference it for flash script patterns, library setup, and sketch structure.

## Python Environment

All Python tools must be installed into the repo's virtual environment — never globally.

```bash
source .venv/bin/activate
.venv/bin/pip install <package>
.venv/bin/esptool.py --port /dev/cu.usbmodem1101 ...
```

## Workflow — New Sketches

**Every new sketch follows these steps in order — all are mandatory:**

1. Write `sketches/<name>/<name>.ino` (plus `User_Setup.h` if using TFT_eSPI)
2. Write `flash-sketch-<NNN>-<name>.sh` (numbered sequentially)
3. Run the flash script and iterate until it compiles and flashes successfully
4. **Commit and push to GitHub** automatically without being asked:

```bash
git add sketches/<name>/ flash-sketch-<NNN>-<name>.sh
git commit -m "Add sketch <NNN>: <description>"
git push
```

Do not consider the task complete until `git push` has succeeded.

The flash script must:
- Install arduino-cli via Homebrew if missing
- Install required libraries (from registry as needed)
- Patch `TFT_eSPI/User_Setup.h` if the sketch uses the display (use the GPIO pins above)
- Compile with `arduino-cli` using the FQBN above
- Flash bootloader + partitions + app via `.venv/bin/esptool.py`
- Support `--compile` and `--flash` flags for partial runs

## Display Library Config (TFT_eSPI User_Setup.h)

Key defines for this board:

```cpp
#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 280
#define TFT_MOSI   -1   // not needed for write-only SPI
#define TFT_SCLK   18
#define TFT_CS     16
#define TFT_DC      2
#define TFT_RST     3
#define TFT_BL     17
#define TFT_BACKLIGHT_ON HIGH
#define SPI_FREQUENCY  40000000
```
