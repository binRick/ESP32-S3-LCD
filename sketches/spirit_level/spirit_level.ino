#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <math.h>

#define PIN_BL  15
#define W       240
#define H       280

// ── Colors ────────────────────────────────────────────────────────────────────
#define BLACK     0x0000
#define WHITE     0xFFFF
#define GRAY      0x7BEF
#define DARKGRAY  0x2945
#define GREEN     0x07E0
#define YELLOW    0xFFE0
#define RED       0xF800
#define CYAN      0x07FF
#define DARKGREEN 0x0320
#define ORANGE    0xFD20

// ── Display ───────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  4, 5, 6, 7, GFX_NOT_DEFINED, SPI2_HOST);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, 8, 0, true, W, H, 0, 20);
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, gfx);

// ── QMI8658 IMU ───────────────────────────────────────────────────────────────
#define IMU_ADDR 0x6B

static void imuInit() {
  auto wr = [](uint8_t r, uint8_t v) {
    Wire.beginTransmission(IMU_ADDR);
    Wire.write(r); Wire.write(v);
    Wire.endTransmission();
  };
  wr(0x02, 0x40);
  wr(0x03, 0x03); // accel 2g, 1000Hz — max precision for leveling
  wr(0x08, 0x01); // accel only
  delay(10);
}

static void imuReadAccel(float &ax, float &ay, float &az) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(0x35);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)6);
  int16_t r[3];
  for (int i = 0; i < 3; i++) {
    uint8_t lo = Wire.read(), hi = Wire.read();
    r[i] = (int16_t)((hi << 8) | lo);
  }
  ax = r[0] / 16384.0f; // 2g → 16384 LSB/g
  ay = r[1] / 16384.0f;
  az = r[2] / 16384.0f;
}

// ── CST816T Touch ─────────────────────────────────────────────────────────────
static bool touchTapped() {
  Wire.beginTransmission(0x15);
  Wire.write(0x02);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)0x15, (uint8_t)1);
  return Wire.available() && Wire.read() > 0;
}

// ── Level state ───────────────────────────────────────────────────────────────
static float smoothPitch = 0, smoothRoll = 0;
static float calPitch = 0, calRoll = 0;
static bool lastTouch = false;
static bool flashGreen = false;
static unsigned long flashMs = 0;

// Bowl geometry
static const int CX = W / 2;
static const int CY = 128;
static const int BOWL_R   = 95;  // outer bowl radius
static const int BUBBLE_R = 14;  // bubble radius
static const float RANGE  = 15.0f; // degrees at bowl edge

// ── Color for current tilt ────────────────────────────────────────────────────
static uint16_t tiltColor(float deg) {
  if (deg < 1.0f)  return GREEN;
  if (deg < 3.0f)  return YELLOW;
  if (deg < 7.0f)  return ORANGE;
  return RED;
}

// ── Draw the level ────────────────────────────────────────────────────────────
static void draw(float pitch, float roll) {
  float totalDeg = sqrtf(pitch*pitch + roll*roll);
  uint16_t color = tiltColor(totalDeg);
  bool isLevel = totalDeg < 1.0f;

  canvas->fillScreen(0x0820); // dark background

  // ── Title / status ────────────────────────────────────────────────────────
  canvas->setTextSize(2);
  if (isLevel && !flashGreen) {
    canvas->setTextColor(GREEN);
    canvas->setCursor(62, 8);
    canvas->print("* LEVEL *");
  } else {
    canvas->setTextColor(GRAY);
    canvas->setCursor(52, 8);
    canvas->print("SPIRIT LEVEL");
  }

  // ── Bowl: concentric rings ────────────────────────────────────────────────
  // Outer ring (thick bezel)
  for (int r = BOWL_R; r <= BOWL_R+3; r++)
    canvas->drawCircle(CX, CY, r, DARKGRAY);

  // Inner fill
  canvas->fillCircle(CX, CY, BOWL_R-1, 0x0410); // dark blue-grey

  // Degree rings at ±5° and ±10°
  for (float deg : {5.0f, 10.0f}) {
    int pr = (int)(deg / RANGE * (BOWL_R - BUBBLE_R - 4));
    canvas->drawCircle(CX, CY, pr, DARKGRAY);
  }

  // Crosshair
  canvas->drawFastHLine(CX - BOWL_R + 6, CY, (BOWL_R - 6) * 2, DARKGRAY);
  canvas->drawFastVLine(CX, CY - BOWL_R + 6, (BOWL_R - 6) * 2, DARKGRAY);

  // Center dot
  canvas->fillCircle(CX, CY, 3, DARKGRAY);

  // ── Color arc on bowl edge ────────────────────────────────────────────────
  for (int r = BOWL_R; r <= BOWL_R+3; r++)
    canvas->drawCircle(CX, CY, r, color);

  // ── Bubble ────────────────────────────────────────────────────────────────
  float bxf = roll  / RANGE * (BOWL_R - BUBBLE_R - 4);
  float byf = pitch / RANGE * (BOWL_R - BUBBLE_R - 4);

  // Clamp bubble inside bowl
  float dist = sqrtf(bxf*bxf + byf*byf);
  float maxD = BOWL_R - BUBBLE_R - 4;
  if (dist > maxD) { bxf = bxf/dist*maxD; byf = byf/dist*maxD; }

  int bx = CX + (int)bxf;
  int by = CY + (int)byf;

  // Bubble shadow
  canvas->fillCircle(bx+2, by+2, BUBBLE_R, 0x0208);
  // Bubble body
  canvas->fillCircle(bx, by, BUBBLE_R, color);
  // Bubble highlight
  canvas->fillCircle(bx-4, by-4, BUBBLE_R/3, canvas->color565(
    255, 255, 255)); // white glint

  // ── Degree readouts ───────────────────────────────────────────────────────
  canvas->setTextSize(2);
  canvas->setTextColor(WHITE);

  char buf[20];
  snprintf(buf, sizeof(buf), "P %+5.1f%c", pitch, 0xDF); // ° = 0xDF in default font? use * instead
  canvas->setCursor(10, 242);
  canvas->print(buf);

  snprintf(buf, sizeof(buf), "R %+5.1f%c", roll, 0xDF);
  canvas->setCursor(W/2 + 4, 242);
  canvas->print(buf);

  // Divider
  canvas->drawFastVLine(W/2, 238, 22, DARKGRAY);

  // ── Calibration hint ──────────────────────────────────────────────────────
  canvas->setTextSize(1);
  canvas->setTextColor(DARKGRAY);
  canvas->setCursor(60, 265);
  canvas->print("TAP TO CALIBRATE");

  canvas->flush();
}

// ── Setup / loop ──────────────────────────────────────────────────────────────
void setup() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  Wire.begin(11, 10);
  Wire.setClock(400000);

  gfx->begin();
  canvas->begin();
  canvas->fillScreen(BLACK);
  canvas->setTextColor(WHITE);
  canvas->setTextSize(2);
  canvas->setCursor(55, 130);
  canvas->print("Spirit Level");
  canvas->flush();
  delay(600);

  imuInit();
}

void loop() {
  float ax, ay, az;
  imuReadAccel(ax, ay, az);

  // Compute pitch/roll from accelerometer
  float rawPitch = atan2f(ay, sqrtf(ax*ax + az*az)) * 180.0f / M_PI;
  float rawRoll  = atan2f(-ax, az) * 180.0f / M_PI;

  // Low-pass filter for smooth bubble — high alpha = very smooth
  const float ALPHA = 0.85f;
  smoothPitch = ALPHA * smoothPitch + (1 - ALPHA) * rawPitch;
  smoothRoll  = ALPHA * smoothRoll  + (1 - ALPHA) * rawRoll;

  // Apply calibration offset
  float pitch = smoothPitch - calPitch;
  float roll  = smoothRoll  - calRoll;

  // Touch = calibrate
  bool touch = touchTapped();
  if (touch && !lastTouch) {
    calPitch = smoothPitch;
    calRoll  = smoothRoll;
    // Flash confirmation
    canvas->fillScreen(DARKGREEN);
    canvas->setTextColor(WHITE);
    canvas->setTextSize(2);
    canvas->setCursor(40, 128);
    canvas->print("CALIBRATED!");
    canvas->flush();
    delay(600);
  }
  lastTouch = touch;

  draw(pitch, roll);
  delay(16); // ~60fps
}
