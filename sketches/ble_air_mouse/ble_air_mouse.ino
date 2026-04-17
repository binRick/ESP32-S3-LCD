#include <Arduino_GFX_Library.h>
#include <BleMouse.h>
#include <Wire.h>
#include <math.h>

#define PIN_BL  15
#define W       240
#define H       280

#define BLACK    0x0000
#define WHITE    0xFFFF
#define RED      0xF800
#define GREEN    0x07E0
#define CYAN     0x07FF
#define YELLOW   0xFFE0
#define GRAY     0x7BEF
#define DARKGRAY 0x2945

// ── Display ───────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  4 /* DC */, 5 /* CS */, 6 /* SCK */, 7 /* MOSI */,
  GFX_NOT_DEFINED, SPI2_HOST);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, 8 /* RST */, 0 /* rotation */, true /* IPS */,
  W, H, 0, 20 /* row offset */);

// ── BLE Mouse ─────────────────────────────────────────────────────────────────
BleMouse bleMouse("ESP32 Air Mouse", "Waveshare", 100);

// ── QMI8658 IMU ───────────────────────────────────────────────────────────────
#define IMU_ADDR  0x6B
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL3 0x04
#define REG_CTRL7 0x08
#define REG_AX_L  0x35

static void imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

static void imuInit() {
  imuWrite(REG_CTRL1, 0x40);
  imuWrite(REG_CTRL2, 0x13); // accel 4g 250Hz
  imuWrite(REG_CTRL3, 0x43); // gyro 256dps 250Hz — lower range = more precision
  imuWrite(REG_CTRL7, 0x03);
  delay(10);
}

static void imuRead(float &gx, float &gy) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(REG_AX_L);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)12);
  int16_t raw[6];
  for (int i = 0; i < 6; i++) {
    uint8_t lo = Wire.read(), hi = Wire.read();
    raw[i] = (int16_t)((hi << 8) | lo);
  }
  // 256dps scale → 128 LSB/dps
  gx = raw[3] / 128.0f;
  gy = raw[4] / 128.0f;
}

// ── CST816T Touch ─────────────────────────────────────────────────────────────
#define TOUCH_ADDR 0x15

static bool touchRead() {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02); // FingerNum register
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)1);
  return Wire.available() && Wire.read() > 0;
}

// ── UI ────────────────────────────────────────────────────────────────────────
static bool lastConnected = false;

static void drawStatusBar(bool connected) {
  gfx->fillRect(0, 0, W, 30, connected ? 0x0320 : 0x3000);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  if (connected) {
    gfx->setCursor(50, 7);
    gfx->print("CONNECTED");
  } else {
    gfx->setCursor(30, 7);
    gfx->print("SEARCHING...");
  }
}

static void drawBackground() {
  gfx->fillRect(0, 30, W, H - 60, BLACK);
  // Grid lines
  for (int x = 0; x < W; x += 40)
    gfx->drawFastVLine(x, 30, H - 60, DARKGRAY);
  for (int y = 30; y < H - 30; y += 40)
    gfx->drawFastHLine(0, y, W, DARKGRAY);
}

static void drawFooter() {
  gfx->fillRect(0, H - 30, W, 30, 0x1082);
  gfx->setTextColor(GRAY);
  gfx->setTextSize(1);
  gfx->setCursor(10, H - 20);
  gfx->print("TILT=MOVE  TAP=CLICK");
}

static void drawCrosshair(int x, int y, uint16_t color) {
  gfx->drawFastHLine(x - 12, y, 25, color);
  gfx->drawFastVLine(x, y - 12, 25, color);
  gfx->drawCircle(x, y, 5, color);
}

// ── State ─────────────────────────────────────────────────────────────────────
static int cx = W / 2, cy = H / 2 + 15; // crosshair position
static bool lastTouch = false;
static uint32_t lastDisplayMs = 0;
static const float SENSITIVITY = 0.35f;
static const float DEAD_ZONE   = 1.5f;   // deg/s

void setup() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  Wire.begin(11, 10);
  Wire.setClock(400000);

  gfx->begin();
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(55, 125);
  gfx->print("Air Mouse");
  gfx->setTextSize(1);
  gfx->setCursor(65, 155);
  gfx->print("Starting BLE...");
  delay(800);

  imuInit();
  bleMouse.begin();

  gfx->fillScreen(BLACK);
  drawStatusBar(false);
  drawBackground();
  drawFooter();
  drawCrosshair(cx, cy, CYAN);
}

void loop() {
  float gx, gy;
  imuRead(gx, gy);

  bool connected = bleMouse.isConnected();

  // Redraw status bar on connect/disconnect
  if (connected != lastConnected) {
    lastConnected = connected;
    drawStatusBar(connected);
    if (connected) {
      drawBackground();
      cx = W / 2; cy = H / 2 + 15;
      drawCrosshair(cx, cy, CYAN);
    }
  }

  if (connected) {
    // Dead zone + scale → mouse delta
    float dx = fabsf(gy) > DEAD_ZONE ? gy * SENSITIVITY : 0;
    float dy = fabsf(gx) > DEAD_ZONE ? gx * SENSITIVITY : 0;
    int8_t mx = (int8_t)constrain((int)dx, -127, 127);
    int8_t my = (int8_t)constrain((int)dy, -127, 127);
    if (mx != 0 || my != 0) bleMouse.move(mx, my);

    // Touch = left click
    bool touch = touchRead();
    if (touch && !lastTouch) {
      bleMouse.click(MOUSE_LEFT);
      drawCrosshair(cx, cy, WHITE);
      delay(60);
      drawCrosshair(cx, cy, CYAN);
    }
    lastTouch = touch;

    // Update crosshair on display (~30fps)
    if (millis() - lastDisplayMs > 33) {
      lastDisplayMs = millis();
      int nx = constrain(cx + mx * 3, 10, W - 10);
      int ny = constrain(cy + my * 3, 40, H - 40);
      if (nx != cx || ny != cy) {
        drawCrosshair(cx, cy, BLACK); // erase old
        // Redraw grid dots where crosshair was
        if ((cx % 40) < 3) gfx->drawFastVLine(cx - (cx % 40), cy - 12 > 30 ? cy - 12 : 30, 25, DARKGRAY);
        cx = nx; cy = ny;
        drawCrosshair(cx, cy, CYAN);
      }
    }
  }

  delay(8); // ~120Hz mouse polling
}
