// mono_face.h — v2 display driver: SSD1306 128x64 1-bit OLED over I2C.
// Implements the display.h interface. The color/round GC9A01 art doesn't
// translate, so the face is re-authored for monochrome low-res: mood comes from
// eye/mouth SHAPE + a few extras (no color, gradients, or particles).
//   I2C:  SDA -> D9 (GPIO8),  SCL -> D8 (GPIO7)   (audit: clear of all sensors/audio)
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

#define OLED_SDA  8
#define OLED_SCL  7
#define OLED_ADDR 0x3C
static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool    mDispReady = false;
static String  mCapText   = "";
static uint32_t mCapUntil = 0;

// expression vocabulary (1-bit equivalents of the v1 styles)
enum { E_NORMAL, E_HAPPY, E_HEART, E_SLEEPY, E_WORRY, E_WIDE, E_TIRED, E_SAD };
enum { M_SMILE, M_GRIN, M_TINY, M_PANT, M_CHATTER, M_FROWN };
enum { L_UP, L_DROOP, L_WILT };
struct MonoCfg { uint8_t eye, mouth, leaf; bool blush, sweat, tear, zzz, shiver; };

static MonoCfg cfgForMono(const String &e) {
  if (e == "happy")   return {E_HAPPY,  M_GRIN,    L_UP,    true,  false, false, false, false};
  if (e == "love")    return {E_HEART,  M_SMILE,   L_UP,    true,  false, false, false, false};
  if (e == "sleepy")  return {E_SLEEPY, M_TINY,    L_DROOP, false, false, false, true,  false};
  if (e == "thirsty") return {E_WORRY,  M_PANT,    L_WILT,  false, true,  false, false, false};
  if (e == "cold")    return {E_WIDE,   M_CHATTER, L_DROOP, true,  false, false, false, true };
  if (e == "hot")     return {E_TIRED,  M_PANT,    L_DROOP, false, true,  false, false, false};
  if (e == "sad")     return {E_SAD,    M_FROWN,   L_WILT,  false, false, true,  false, false};
  if (e == "offline") return {E_WORRY,  M_TINY,    L_DROOP, false, false, false, false, false};
  return {E_NORMAL, M_SMILE, L_UP, false, false, false, false, false};   // neutral
}

// stroke an arc as short line segments (Adafruit_GFX has no arc primitive)
static void mArc(int cx, int cy, int r, float a0, float a1, uint16_t col) {
  const int n = 9; float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
  for (int i = 1; i <= n; i++) { float a = a0 + (a1 - a0) * i / n;
    float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
    oled.drawLine((int)px, (int)py, (int)x, (int)y, col); px = x; py = y; }
}
static void mHeart(int x, int y, int s) {
  oled.fillCircle(x - s / 2, y - 1, (s + 1) / 2, SSD1306_WHITE);
  oled.fillCircle(x + s / 2, y - 1, (s + 1) / 2, SSD1306_WHITE);
  oled.fillTriangle(x - s, y, x + s, y, x, y + s, SSD1306_WHITE);
}
static void mDrop(int x, int y) {
  oled.fillTriangle(x, y - 3, x - 2, y, x + 2, y, SSD1306_WHITE);
  oled.fillCircle(x, y + 1, 2, SSD1306_WHITE);
}

static void mEye(int x, int y, uint8_t style, float open) {
  bool openType = (style == E_NORMAL || style == E_WIDE || style == E_HEART ||
                   style == E_WORRY || style == E_TIRED || style == E_SAD);
  if (openType && open < 0.18f) { oled.drawFastHLine(x - 4, y, 8, SSD1306_WHITE); return; }  // blink
  switch (style) {
    case E_NORMAL: oled.fillCircle(x, y, 3, SSD1306_WHITE); break;
    case E_WIDE:   oled.drawCircle(x, y, 4, SSD1306_WHITE); oled.fillCircle(x, y, 2, SSD1306_WHITE); break;
    case E_HAPPY:  mArc(x, y + 2, 5, PI * 1.15f, PI * 1.85f, SSD1306_WHITE); break;   // ^ curve
    case E_HEART:  mHeart(x, y, 5); break;
    case E_SLEEPY: mArc(x, y - 1, 5, PI * 0.15f, PI * 0.85f, SSD1306_WHITE); break;   // droopy lid
    case E_WORRY:  oled.fillCircle(x, y + 1, 2, SSD1306_WHITE); oled.drawLine(x - 4, y - 4, x + 3, y - 2, SSD1306_WHITE); break;
    case E_TIRED:  oled.fillCircle(x, y, 3, SSD1306_WHITE); oled.fillRect(x - 4, y - 4, 8, 4, SSD1306_BLACK);
                   oled.drawFastHLine(x - 4, y - 1, 8, SSD1306_WHITE); break;         // half-lidded
    case E_SAD:    oled.fillCircle(x, y + 1, 2, SSD1306_WHITE); oled.drawLine(x - 4, y - 2, x + 3, y - 5, SSD1306_WHITE); break;  // inner-up brow
  }
}

static void mMouth(int x, int y, uint8_t style) {
  switch (style) {
    case M_SMILE:   mArc(x, y - 4, 7, PI * 0.15f, PI * 0.85f, SSD1306_WHITE); break;
    case M_GRIN:    mArc(x, y - 4, 8, PI * 0.05f, PI * 0.95f, SSD1306_WHITE); oled.drawFastHLine(x - 8, y - 4, 16, SSD1306_WHITE); break;
    case M_TINY:    oled.fillCircle(x, y, 1, SSD1306_WHITE); break;
    case M_PANT:    oled.fillCircle(x, y, 3, SSD1306_WHITE); break;
    case M_CHATTER: { int px = x - 8, py = y; for (int i = 0; i <= 4; i++) {
                      int nx = x - 8 + i * 4, ny = y + ((i % 2) ? 3 : -3);
                      if (i) oled.drawLine(px, py, nx, ny, SSD1306_WHITE); px = nx; py = ny; } break; }
    case M_FROWN:   mArc(x, y + 4, 7, PI * 1.15f, PI * 1.85f, SSD1306_WHITE); break;
  }
}

static void drawMonoFace(const String &emoIn, bool offline, uint32_t t) {
  oled.clearDisplay();
  String emo = offline ? String("offline") : emoIn;
  MonoCfg f = cfgForMono(emo);

  // deterministic blink: a ~130ms blink at the top of each 3.2s cycle
  uint32_t c = t % 3200;
  float open = (c < 130) ? (c < 65 ? 1.0f - c / 65.0f : (c - 65) / 65.0f) : 1.0f;

  int bob = (int)lroundf(sinf(t / 700.0f));                 // gentle breathe
  int shv = f.shiver ? (int)lroundf(sinf(t / 45.0f) * 1.5f) : 0;
  int cx = 64 + shv, cy = 32 + bob;
  int topY = cy - 19;

  if (!offline) {                                            // sprout + leaves
    int droop = (f.leaf == L_WILT) ? 4 : (f.leaf == L_DROOP ? 2 : 0);
    oled.drawFastVLine(cx, topY - 6, 6, SSD1306_WHITE);
    oled.fillTriangle(cx, topY - 6, cx - 6, topY - 8 + droop, cx - 1, topY - 12 + droop, SSD1306_WHITE);
    oled.fillTriangle(cx, topY - 6, cx + 6, topY - 8 + droop, cx + 1, topY - 12 + droop, SSD1306_WHITE);
  }

  oled.drawRoundRect(cx - 27, cy - 19, 54, 38, 13, SSD1306_WHITE);   // body outline

  int eyY = cy - 3;
  mEye(cx - 13, eyY, f.eye, open);
  mEye(cx + 13, eyY, f.eye, open);
  mMouth(cx, cy + 9, f.mouth);

  if (f.blush) { oled.drawCircle(cx - 18, cy + 4, 2, SSD1306_WHITE); oled.drawCircle(cx + 18, cy + 4, 2, SSD1306_WHITE); }
  if (f.sweat) mDrop(cx + 22, cy - 6 + (int)(sinf(t / 200.0f)));
  if (f.tear)  mDrop(cx - 16, cy + 2 + (int)((t / 12) % 12));
  if (f.shiver) { oled.drawFastVLine(cx - 32, cy - 6, 8, SSD1306_WHITE); oled.drawFastVLine(cx + 32, cy - 6, 8, SSD1306_WHITE); }
  if (f.zzz) { int zb = (int)(sinf(t / 450.0f) * 2);
    oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE); oled.setCursor(cx + 20, topY - 2 + zb); oled.print("z");
    oled.setTextSize(2); oled.setCursor(cx + 26, topY - 8 - zb); oled.print("Z"); }

  if (offline) {                                            // "looking for internet" rings
    int wcy = 7; uint32_t s = (t / 300) % 4;
    for (int k = 0; k < 3; k++) if (s > (uint32_t)(k + 1)) mArc(cx, wcy, 3 + k * 4, PI * 1.25f, PI * 1.75f, SSD1306_WHITE);
    oled.fillCircle(cx, wcy, 1, SSD1306_WHITE);
  }

  if (mCapText.length() && millis() < mCapUntil) {          // straight caption line at the bottom
    oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
    String s = mCapText.length() > 21 ? mCapText.substring(0, 20) + "." : mCapText;
    int w = s.length() * 6, x = (128 - w) / 2; if (x < 0) x = 0;
    oled.setCursor(x, 56); oled.print(s);
  }

  oled.display();
}

// ---- display interface implementation (v2 driver: SSD1306 mono) ----
void displaySetup() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {   // periphBegin=false: keep our I2C pins
    Serial.println("[DISP] SSD1306 init FAILED (check SDA=D9/SCL=D8/VCC/GND)");
    return;
  }
  mDispReady = true;
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2); oled.setCursor(26, 22); oled.print("Terra");
  oled.display();
  Serial.println("[DISP] SSD1306 128x64 ready");
}

void displayRenderFace(const String &emotion, bool offline, uint32_t t) {
  if (!mDispReady) return;
  static uint32_t last = 0;
  if (t - last < 66) return;              // ~15 fps cap — plenty for a kawaii face, easy on I2C
  last = t;
  drawMonoFace(emotion, offline, t);
}

void displaySetCaption(const String &text, uint32_t ms) {
  mCapText = text; mCapUntil = millis() + ms;
}
