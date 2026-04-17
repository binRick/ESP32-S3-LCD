#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <math.h>

#define PIN_BL   15
#define W        240
#define H        280

#define BLACK   0x0000
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define CYAN    0x07FF
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

// ── Display (Arduino_GFX) ─────────────────────────────────────────────────────
// Row offset = 20: ST7789V2 240×280 sits at rows 20–299 in a 320-row controller
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  4 /* DC */, 5 /* CS */, 6 /* SCK */, 7 /* MOSI */,
  GFX_NOT_DEFINED /* MISO */, SPI2_HOST);
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, 8 /* RST */, 0 /* rotation */, true /* IPS */,
  W, H, 0 /* col_offset */, 20 /* row_offset */);
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, gfx);

// ── QMI8658 IMU ───────────────────────────────────────────────────────────────
#define IMU_ADDR  0x6B
#define REG_WHO   0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL3 0x04
#define REG_CTRL7 0x08
#define REG_AX_L  0x35

static void imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool imuInit() {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(REG_WHO);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)1);
  if (!Wire.available() || Wire.read() != 0x05) return false;
  imuWrite(REG_CTRL1, 0x40); // address auto-increment
  imuWrite(REG_CTRL2, 0x13); // accel 4g, 250Hz
  imuWrite(REG_CTRL3, 0x53); // gyro 512dps, 250Hz
  imuWrite(REG_CTRL7, 0x03); // enable accel + gyro
  delay(10);
  return true;
}

static void imuRead(float &ax, float &ay, float &az,
                    float &gx, float &gy, float &gz) {
  Wire.beginTransmission(IMU_ADDR);
  Wire.write(REG_AX_L);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)IMU_ADDR, (uint8_t)12);
  int16_t raw[6];
  for (int i = 0; i < 6; i++) {
    uint8_t lo = Wire.read(), hi = Wire.read();
    raw[i] = (int16_t)((hi << 8) | lo);
  }
  ax = raw[0] / 8192.0f;
  ay = raw[1] / 8192.0f;
  az = raw[2] / 8192.0f;
  gx = raw[3] / 64.0f;
  gy = raw[4] / 64.0f;
  gz = raw[5] / 64.0f;
}

// ── Complementary filter ──────────────────────────────────────────────────────
static float pitch = 0, roll = 0, yaw = 0;
static unsigned long lastUs = 0;
static const float ALPHA = 0.96f;

static void updateAngles(float ax, float ay, float az,
                         float gx, float gy, float gz) {
  unsigned long now = micros();
  float dt = (now - lastUs) * 1e-6f;
  if (dt > 0.1f || dt < 0) dt = 0.01f;
  lastUs = now;
  float accPitch = atan2f(ay, sqrtf(ax * ax + az * az));
  float accRoll  = atan2f(-ax, az);
  pitch = ALPHA * (pitch + gx * (M_PI / 180.0f) * dt) + (1 - ALPHA) * accPitch;
  roll  = ALPHA * (roll  + gy * (M_PI / 180.0f) * dt) + (1 - ALPHA) * accRoll;
  yaw  += gz * (M_PI / 180.0f) * dt;
}

// ── 3D Cube ───────────────────────────────────────────────────────────────────
static const float VERTS[8][3] = {
  {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
  {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
};
static const uint8_t EDGES[12][2] = {
  {0,1},{1,2},{2,3},{3,0},
  {4,5},{5,6},{6,7},{7,4},
  {0,4},{1,5},{2,6},{3,7}
};

static inline void rotateVert(float x, float y, float z,
                               float rx, float ry, float rz,
                               float &ox, float &oy, float &oz) {
  float y1 = y * cosf(rx) - z * sinf(rx);
  float z1 = y * sinf(rx) + z * cosf(rx);
  float x2 = x * cosf(ry) + z1 * sinf(ry);
  float z2 = -x * sinf(ry) + z1 * cosf(ry);
  ox = x2 * cosf(rz) - y1 * sinf(rz);
  oy = x2 * sinf(rz) + y1 * cosf(rz);
  oz = z2;
}

static inline void project(float x, float y, float z, int &sx, int &sy) {
  float d = z + 4.0f;
  if (d < 0.1f) d = 0.1f;
  sx = (int)(x * 90.0f / d) + W / 2;
  sy = (int)(y * 90.0f / d) + H / 2;
}

static uint16_t depthColor(float depth, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t br = (uint8_t)((depth + 2.0f) * 50.0f + 55.0f);
  return canvas->color565(r ? br : 0, g ? br : 0, b ? br : 0);
}

static void drawCube(float rx, float ry, float rz) {
  canvas->fillScreen(BLACK);

  float vx[8], vy[8], vz[8];
  int sx[8], sy[8];
  for (int i = 0; i < 8; i++) {
    rotateVert(VERTS[i][0], VERTS[i][1], VERTS[i][2], rx, ry, rz,
               vx[i], vy[i], vz[i]);
    project(vx[i], vy[i], vz[i], sx[i], sy[i]);
  }
  for (int e = 0; e < 12; e++) {
    int a = EDGES[e][0], b = EDGES[e][1];
    float depth = (vz[a] + vz[b]) * 0.5f;
    canvas->drawLine(sx[a], sy[a], sx[b], sy[b], depthColor(depth, 0, 1, 1));
  }
  for (int i = 0; i < 8; i++) {
    canvas->fillCircle(sx[i], sy[i], 3, depthColor(vz[i], 1, 1, 0));
  }
  canvas->flush();
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  Wire.begin(11, 10);
  Wire.setClock(400000);

  gfx->begin();
  canvas->begin();

  // Brief startup flash to confirm display is alive
  gfx->fillScreen(RED);   delay(300);
  gfx->fillScreen(GREEN); delay(300);
  gfx->fillScreen(BLUE);  delay(300);
  gfx->fillScreen(BLACK);

  imuInit();
  lastUs = micros();
}

void loop() {
  float ax, ay, az, gx, gy, gz;
  imuRead(ax, ay, az, gx, gy, gz);
  updateAngles(ax, ay, az, gx, gy, gz);
  drawCube(pitch, roll, yaw);
}
