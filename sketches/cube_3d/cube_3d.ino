#include <TFT_eSPI.h>
#include <Wire.h>
#include <math.h>

#define PIN_BL   15
#define W        240
#define H        280

// ── QMI8658 IMU ──────────────────────────────────────────────────────────────
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
  ax = raw[0] / 8192.0f; // 4g scale → LSB/g = 8192
  ay = raw[1] / 8192.0f;
  az = raw[2] / 8192.0f;
  gx = raw[3] / 64.0f;   // 512dps scale → LSB/dps = 64
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

TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);

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

static void drawCube(float rx, float ry, float rz) {
  spr.fillSprite(TFT_BLACK);

  float vx[8], vy[8], vz[8];
  int sx[8], sy[8];
  for (int i = 0; i < 8; i++) {
    rotateVert(VERTS[i][0], VERTS[i][1], VERTS[i][2], rx, ry, rz,
               vx[i], vy[i], vz[i]);
    project(vx[i], vy[i], vz[i], sx[i], sy[i]);
  }

  // Edges — cyan gradient by depth
  for (int e = 0; e < 12; e++) {
    int a = EDGES[e][0], b = EDGES[e][1];
    float depth = (vz[a] + vz[b]) * 0.5f;
    uint8_t br = (uint8_t)((depth + 2.0f) * 50.0f + 55.0f);
    spr.drawLine(sx[a], sy[a], sx[b], sy[b], spr.color565(0, br, br));
  }

  // Vertices — yellow dots
  for (int i = 0; i < 8; i++) {
    uint8_t br = (uint8_t)((vz[i] + 2.0f) * 50.0f + 55.0f);
    spr.fillCircle(sx[i], sy[i], 3, spr.color565(br, br, 0));
  }

  spr.pushSprite(0, 0);
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  Wire.begin(11, 10); // SDA=11, SCL=10
  Wire.setClock(400000);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  spr.createSprite(W, H);
  spr.setColorDepth(16);

  imuInit();
  lastUs = micros();
}

void loop() {
  float ax, ay, az, gx, gy, gz;
  imuRead(ax, ay, az, gx, gy, gz);
  updateAngles(ax, ay, az, gx, gy, gz);
  drawCube(pitch, roll, yaw);
}
