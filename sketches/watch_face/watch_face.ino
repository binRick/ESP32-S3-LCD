#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <math.h>

// ── Hardware ──────────────────────────────────────────────────────────────────
#define PIN_BL   15
#define W        240
#define H        280

// ── Colors ────────────────────────────────────────────────────────────────────
#define BLACK     0x0000
#define WHITE     0xFFFF
#define RED       0xF800
#define GREEN     0x07E0
#define CYAN      0x07FF
#define YELLOW    0xFFE0
#define GOLD      0xFEA0
#define GRAY      0x7BEF
#define DARKGRAY  0x2945
#define NAVY      0x000F
#define DARKBLUE  0x0410
#define ORANGE    0xFD20

// ── Display ───────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  4, 5, 6, 7, GFX_NOT_DEFINED, SPI2_HOST);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, 8, 0, true, W, H, 0, 20);
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, gfx);

// ── PCF85063 RTC ──────────────────────────────────────────────────────────────
#define RTC_ADDR 0x51

struct TimeData { uint8_t hour, min, sec, day, month, year, wday; };

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

static void rtcWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

static TimeData rtcRead() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7);
  TimeData t;
  t.sec   = bcd2dec(Wire.read() & 0x7F);
  t.min   = bcd2dec(Wire.read() & 0x7F);
  t.hour  = bcd2dec(Wire.read() & 0x3F);
  t.day   = bcd2dec(Wire.read() & 0x3F);
  t.wday  = Wire.read() & 0x07;
  t.month = bcd2dec(Wire.read() & 0x1F);
  t.year  = bcd2dec(Wire.read());
  return t;
}

static uint8_t compileMonth() {
  const char *m = __DATE__;
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (int i = 0; i < 12; i++)
    if (strncmp(m, months + i * 3, 3) == 0) return i + 1;
  return 1;
}

static void rtcSetFromCompile() {
  // __DATE__ = "Apr 17 2026", __TIME__ = "18:30:00"
  uint8_t h  = (__TIME__[0]-'0')*10 + (__TIME__[1]-'0');
  uint8_t mn = (__TIME__[3]-'0')*10 + (__TIME__[4]-'0');
  uint8_t s  = (__TIME__[6]-'0')*10 + (__TIME__[7]-'0');
  uint8_t dy = (__DATE__[4]==' ' ? 0 : __DATE__[4]-'0')*10 + (__DATE__[5]-'0');
  uint8_t mo = compileMonth();
  uint8_t yr = (__DATE__[9]-'0')*10 + (__DATE__[10]-'0');

  rtcWrite(0x00, 0x20); // STOP
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.write(dec2bcd(s));
  Wire.write(dec2bcd(mn));
  Wire.write(dec2bcd(h));
  Wire.write(dec2bcd(dy));
  Wire.write(0); // weekday
  Wire.write(dec2bcd(mo));
  Wire.write(dec2bcd(yr));
  Wire.endTransmission();
  rtcWrite(0x00, 0x00); // START
}

static bool rtcNeedsSet() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)1);
  return Wire.available() && (Wire.read() & 0x80); // OS flag
}

// ── QMI8658 IMU ───────────────────────────────────────────────────────────────
#define IMU_ADDR  0x6B

static void imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}

static void imuInit() {
  imuWrite(0x02, 0x40);
  imuWrite(0x03, 0x13); // accel 4g 250Hz
  imuWrite(0x04, 0x53); // gyro 512dps
  imuWrite(0x08, 0x03);
  delay(10);
}

static void imuReadAll(float &ax, float &ay, float &az,
                       float &gx, float &gy, float &gz) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(0x35);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)12);
  int16_t r[6];
  for (int i = 0; i < 6; i++) {
    uint8_t lo = Wire.read(), hi = Wire.read();
    r[i] = (int16_t)((hi << 8) | lo);
  }
  ax = r[0]/8192.0f; ay = r[1]/8192.0f; az = r[2]/8192.0f;
  gx = r[3]/64.0f;   gy = r[4]/64.0f;   gz = r[5]/64.0f;
}

// ── CST816T Touch ─────────────────────────────────────────────────────────────
static bool touchTapped() {
  Wire.beginTransmission(0x15);
  Wire.write(0x01); // GestureID
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)0x15, (uint8_t)2);
  if (!Wire.available()) return false;
  uint8_t gesture = Wire.read();
  uint8_t fingers = Wire.available() ? Wire.read() : 0;
  return gesture == 0x01 || fingers > 0; // 0x01 = single tap gesture
}

// ── Watch state ───────────────────────────────────────────────────────────────
static int faceIdx = 0;
static bool awake = true;
static unsigned long lastActivityMs = 0;
static unsigned long lastSecMs = 0;
static TimeData lastTime = {0};
static bool lastTouch = false;
static float pitchAngle = 0, rollAngle = 0;

static const char *DAYS[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
static const char *MONTHS[]  = {"","JAN","FEB","MAR","APR","MAY","JUN",
                                 "JUL","AUG","SEP","OCT","NOV","DEC"};

// ── Drawing helpers ───────────────────────────────────────────────────────────
static void thickLine(int x0, int y0, int x1, int y1,
                      int thick, uint16_t color) {
  for (int d = -thick/2; d <= thick/2; d++) {
    float dx = x1-x0, dy = y1-y0;
    float len = sqrtf(dx*dx+dy*dy);
    if (len < 1) continue;
    int ox = (int)(-dy/len*d), oy = (int)(dx/len*d);
    canvas->drawLine(x0+ox, y0+oy, x1+ox, y1+oy, color);
  }
}

// ── Face 0: Digital ───────────────────────────────────────────────────────────
static void drawDigital(const TimeData &t) {
  canvas->fillScreen(0x0008); // very dark blue

  // Time
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
  canvas->setTextColor(CYAN);
  canvas->setTextSize(6);
  int tw = strlen(buf) * 6 * 6;
  canvas->setCursor((W - tw) / 2, 70);
  canvas->print(buf);

  // Seconds
  snprintf(buf, sizeof(buf), "%02d", t.sec);
  canvas->setTextColor(GOLD);
  canvas->setTextSize(3);
  canvas->setCursor((W - 6*3*2)/2, 148);
  canvas->print(buf);

  // Seconds progress bar
  int barW = 200, barH = 6;
  int barX = (W - barW) / 2, barY = 178;
  canvas->fillRect(barX, barY, barW, barH, DARKGRAY);
  int fill = (int)(t.sec / 59.0f * barW);
  canvas->fillRect(barX, barY, fill, barH, CYAN);

  // Date
  canvas->setTextColor(WHITE);
  canvas->setTextSize(2);
  char dateBuf[16];
  snprintf(dateBuf, sizeof(dateBuf), "%s %02d %s",
    DAYS[t.wday % 7], t.day, MONTHS[t.month % 13]);
  int dw = strlen(dateBuf) * 6 * 2;
  canvas->setCursor((W - dw) / 2, 210);
  canvas->print(dateBuf);

  // Face indicator dots
  for (int i = 0; i < 3; i++) {
    uint16_t c = (i == faceIdx) ? WHITE : DARKGRAY;
    canvas->fillCircle(W/2 - 12 + i*12, 255, 3, c);
  }

  canvas->flush();
}

// ── Face 1: Analog ────────────────────────────────────────────────────────────
static const int CX = W/2, CY = 135, CR = 108;

static void drawAnalog(const TimeData &t) {
  canvas->fillScreen(BLACK);

  // Bezel rings
  canvas->drawCircle(CX, CY, CR+3, GOLD);
  canvas->drawCircle(CX, CY, CR+4, GOLD);

  // Tick marks
  for (int i = 0; i < 60; i++) {
    float a = (i / 60.0f) * 2*M_PI - M_PI/2;
    bool major = (i % 5 == 0);
    int r1 = CR, r2 = major ? CR-14 : CR-7;
    canvas->drawLine(
      CX + (int)(r1*cosf(a)), CY + (int)(r1*sinf(a)),
      CX + (int)(r2*cosf(a)), CY + (int)(r2*sinf(a)),
      major ? WHITE : DARKGRAY);
    if (major) canvas->drawLine(
      CX + (int)((r1-1)*cosf(a)), CY + (int)((r1-1)*sinf(a)),
      CX + (int)((r2)*cosf(a)),   CY + (int)((r2)*sinf(a)),
      WHITE);
  }

  // Hour numbers
  canvas->setTextSize(1);
  canvas->setTextColor(WHITE);
  const char *nums[] = {"12","1","2","3","4","5","6","7","8","9","10","11"};
  for (int i = 0; i < 12; i++) {
    float a = (i / 12.0f) * 2*M_PI - M_PI/2;
    int r = CR - 24;
    int nx = CX + (int)(r*cosf(a)) - (strlen(nums[i])==1 ? 3 : 6);
    int ny = CY + (int)(r*sinf(a)) - 4;
    canvas->setCursor(nx, ny);
    canvas->print(nums[i]);
  }

  // Hour hand
  float ha = ((t.hour%12) + t.min/60.0f) / 12.0f * 2*M_PI - M_PI/2;
  thickLine(CX, CY,
    CX + (int)(CR*0.48f*cosf(ha)),
    CY + (int)(CR*0.48f*sinf(ha)), 4, WHITE);

  // Minute hand
  float ma = (t.min + t.sec/60.0f) / 60.0f * 2*M_PI - M_PI/2;
  thickLine(CX, CY,
    CX + (int)(CR*0.72f*cosf(ma)),
    CY + (int)(CR*0.72f*sinf(ma)), 3, WHITE);

  // Second hand
  float sa = t.sec / 60.0f * 2*M_PI - M_PI/2;
  canvas->drawLine(CX, CY,
    CX + (int)(CR*0.82f*cosf(sa)),
    CY + (int)(CR*0.82f*sinf(sa)), RED);
  // Tail
  canvas->drawLine(CX, CY,
    CX - (int)(CR*0.2f*cosf(sa)),
    CY - (int)(CR*0.2f*sinf(sa)), RED);

  // Center dot
  canvas->fillCircle(CX, CY, 5, RED);
  canvas->fillCircle(CX, CY, 2, WHITE);

  // Date window at bottom
  char dateBuf[12];
  snprintf(dateBuf, sizeof(dateBuf), "%s %02d", MONTHS[t.month%13], t.day);
  canvas->fillRect(CX-22, CY+CR-30, 44, 16, DARKBLUE);
  canvas->setTextColor(WHITE);
  canvas->setTextSize(1);
  canvas->setCursor(CX - strlen(dateBuf)*3, CY+CR-26);
  canvas->print(dateBuf);

  // Face dots
  for (int i = 0; i < 3; i++) {
    uint16_t c = (i == faceIdx) ? WHITE : DARKGRAY;
    canvas->fillCircle(W/2 - 12 + i*12, 263, 3, c);
  }

  canvas->flush();
}

// ── Face 2: Info / Level ──────────────────────────────────────────────────────
static void drawInfo(const TimeData &t, float pitch, float roll) {
  canvas->fillScreen(0x0008);

  // Time (smaller)
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
  canvas->setTextColor(CYAN);
  canvas->setTextSize(4);
  int tw = strlen(buf)*6*4;
  canvas->setCursor((W-tw)/2, 18);
  canvas->print(buf);

  // Divider
  canvas->drawFastHLine(20, 55, W-40, DARKGRAY);

  // Level bubble display
  canvas->setTextColor(GRAY);
  canvas->setTextSize(1);
  canvas->setCursor(10, 65);
  canvas->print("TILT LEVEL");

  // Horizontal level bar (roll)
  int lx = 20, ly = 82, lw = W-40, lh = 18;
  canvas->fillRoundRect(lx, ly, lw, lh, 4, DARKGRAY);
  int bpos = constrain((int)(lw/2 + roll/45.0f * lw/2), 0, lw-12);
  uint16_t bc = (fabsf(roll) < 3) ? GREEN : ORANGE;
  canvas->fillRoundRect(lx+bpos, ly+2, 12, lh-4, 3, bc);
  canvas->drawFastVLine(lx+lw/2, ly-3, lh+6, WHITE); // center mark

  canvas->setTextSize(1);
  canvas->setTextColor(GRAY);
  canvas->setCursor(10, 106);
  char rb[16]; snprintf(rb, sizeof(rb), "ROLL  %+.1f deg", roll);
  canvas->print(rb);

  // Vertical level bar (pitch)
  canvas->setCursor(10, 122);
  canvas->print("PITCH LEVEL");

  int px2 = 20, py2 = 138, pw2 = W-40, ph2 = 18;
  canvas->fillRoundRect(px2, py2, pw2, ph2, 4, DARKGRAY);
  int ppx = constrain((int)(pw2/2 + pitch/45.0f * pw2/2), 0, pw2-12);
  uint16_t pc = (fabsf(pitch) < 3) ? GREEN : ORANGE;
  canvas->fillRoundRect(px2+ppx, py2+2, 12, ph2-4, 3, pc);
  canvas->drawFastVLine(px2+pw2/2, py2-3, ph2+6, WHITE);

  char pb[16]; snprintf(pb, sizeof(pb), "PITCH %+.1f deg", pitch);
  canvas->setTextColor(GRAY);
  canvas->setCursor(10, 162);
  canvas->print(pb);

  // Divider
  canvas->drawFastHLine(20, 178, W-40, DARKGRAY);

  // Date
  char dateBuf[20];
  snprintf(dateBuf, sizeof(dateBuf), "%s  %02d %s  20%02d",
    DAYS[t.wday%7], t.day, MONTHS[t.month%13], t.year);
  canvas->setTextColor(WHITE);
  canvas->setTextSize(1);
  canvas->setCursor((W - (int)strlen(dateBuf)*6)/2, 188);
  canvas->print(dateBuf);

  // Face dots
  for (int i = 0; i < 3; i++) {
    uint16_t c = (i == faceIdx) ? WHITE : DARKGRAY;
    canvas->fillCircle(W/2 - 12 + i*12, 255, 3, c);
  }

  canvas->flush();
}

// ── Raise-to-wake ─────────────────────────────────────────────────────────────
static float prevAz = 0;
static bool checkRaiseToWake(float az) {
  bool raised = (prevAz < 0.4f && az > 0.7f); // wrist raised
  prevAz = az * 0.8f + prevAz * 0.2f;          // smooth
  return raised;
}

static void setBrightness(bool on) {
  digitalWrite(PIN_BL, on ? HIGH : LOW);
}

// ── Arduino ───────────────────────────────────────────────────────────────────
void setup() {
  pinMode(PIN_BL, OUTPUT);
  setBrightness(true);

  Wire.begin(11, 10);
  Wire.setClock(400000);

  gfx->begin();
  canvas->begin();
  canvas->fillScreen(BLACK);
  canvas->setTextColor(WHITE);
  canvas->setTextSize(2);
  canvas->setCursor(60, 130);
  canvas->print("Watch Face");
  canvas->flush();

  imuInit();

  if (rtcNeedsSet()) rtcSetFromCompile();

  lastActivityMs = millis();
  lastSecMs = millis();
}

void loop() {
  float ax, ay, az, gx, gy, gz;
  imuReadAll(ax, ay, az, gx, gy, gz);

  // Raise-to-wake
  if (checkRaiseToWake(az)) {
    if (!awake) { awake = true; setBrightness(true); }
    lastActivityMs = millis();
  }

  // Sleep after timeout
  if (awake && millis() - lastActivityMs > 10000) {
    awake = false;
    setBrightness(false);
  }

  // Touch → cycle face + wake
  bool touch = touchTapped();
  if (touch && !lastTouch) {
    faceIdx = (faceIdx + 1) % 3;
    awake = true;
    setBrightness(true);
    lastActivityMs = millis();
    lastTime.sec = 99; // force redraw
  }
  lastTouch = touch;

  // Update pitch/roll for info face
  pitchAngle = atan2f(ay, sqrtf(ax*ax + az*az)) * 180.0f / M_PI;
  rollAngle  = atan2f(-ax, az) * 180.0f / M_PI;

  // Redraw every second (or immediately on face change)
  if (millis() - lastSecMs >= 1000) {
    lastSecMs = millis();
    TimeData t = rtcRead();
    if (awake) {
      if      (faceIdx == 0) drawDigital(t);
      else if (faceIdx == 1) drawAnalog(t);
      else                   drawInfo(t, pitchAngle, rollAngle);
    }
    lastTime = t;
  }

  delay(50);
}
