// mono_face.h — v2 display driver: SSD1306 128x64 1-bit OLED over I2C.
// "Terra eyes" — big, bold, cute full-screen eyes that own the whole display
// (no bounding box). The mood lives in the eye shapes; a tiny sprout sprig up
// top keeps her a plant. Properly animated: blink + double-blink, idle glance,
// breathe, mood pop, sway, and per-mood extras. Implements display.h.
//   I2C:  SDA -> D9 (GPIO8),  SCL -> D8 (GPIO7)
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

#define OLED_SDA  8
#define OLED_SCL  7
#define OLED_ADDR 0x3C
static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool     mDispReady = false;
static String   mCapText   = "";
static uint32_t mCapUntil  = 0;

enum { EM_NORMAL, EM_HAPPY, EM_HEART, EM_SLEEPY, EM_TIRED, EM_WORRY, EM_SAD };
static uint8_t eyeMood(const String &e) {
  if (e == "happy")   return EM_HAPPY;
  if (e == "love")    return EM_HEART;
  if (e == "sleepy")  return EM_SLEEPY;
  if (e == "hot")     return EM_TIRED;
  if (e == "thirsty") return EM_WORRY;
  if (e == "sad")     return EM_SAD;
  return EM_NORMAL;                      // neutral, cold, offline
}

enum { BR_FLAT, BR_RAISED, BR_WORRIED, BR_LOW };
static uint8_t browMode(const String &e) {
  if (e == "happy" || e == "love" || e == "cold")     return BR_RAISED;   // pleased / alert
  if (e == "sad" || e == "thirsty" || e == "offline") return BR_WORRIED;  // inner-up concern
  if (e == "hot" || e == "sleepy")                    return BR_LOW;      // relaxed / droopy
  return BR_FLAT;
}

static void mDrop(int x, int y) {
  oled.fillTriangle(x, y - 4, x - 2, y, x + 2, y, SSD1306_WHITE);
  oled.fillCircle(x, y + 1, 2, SSD1306_WHITE);
}
static void bigHeart(int cx, int cy, int s) {
  oled.fillCircle(cx - s / 2, cy - s / 3, (s + 1) / 2, SSD1306_WHITE);
  oled.fillCircle(cx + s / 2, cy - s / 3, (s + 1) / 2, SSD1306_WHITE);
  oled.fillTriangle(cx - s, cy - s / 4, cx + s, cy - s / 4, cx, cy + s, SSD1306_WHITE);
  oled.fillCircle(cx - s / 2, cy - s / 3, 2, SSD1306_BLACK);          // glossy sparkle
}
static void mArc(int cx, int cy, int r, float a0, float a1) {
  const int n = 10; float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
  for (int i = 1; i <= n; i++) { float a = a0 + (a1 - a0) * i / n;
    float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
    oled.drawLine((int)px, (int)py, (int)x, (int)y, SSD1306_WHITE); px = x; py = y; }
}
static void thickLine(int x0, int y0, int x1, int y1) {              // 3px brow stroke
  for (int d = 0; d < 3; d++) oled.drawLine(x0, y0 + d, x1, y1 + d, SSD1306_WHITE);
}
static void drawBrows(int lxc, int rxc, int by, uint8_t mode) {
  const int hw = 13;
  switch (mode) {
    case BR_RAISED:                                   // arched up ∩ (pleased/alert)
      thickLine(lxc - hw, by - 1, lxc, by - 4); thickLine(lxc, by - 4, lxc + hw, by - 1);
      thickLine(rxc - hw, by - 1, rxc, by - 4); thickLine(rxc, by - 4, rxc + hw, by - 1); break;
    case BR_WORRIED:                                  // inner ends up (concern)
      thickLine(lxc - hw, by + 3, lxc + hw, by - 3);
      thickLine(rxc - hw, by - 3, rxc + hw, by + 3); break;
    case BR_LOW:                                      // relaxed / drooped
      thickLine(lxc - hw, by + 3, lxc + hw, by + 1);
      thickLine(rxc - hw, by + 1, rxc + hw, by + 3); break;
    default:                                          // BR_FLAT
      thickLine(lxc - hw, by, lxc + hw, by); thickLine(rxc - hw, by, rxc + hw, by); break;
  }
}

// one big CUTE eye: rounded + glossy sparkle catchlights (hlx/hly nudge the
// shine opposite the glance). top-left x, vertical center cyc, width w, height h.
static void drawEye(int x, int cyc, int w, int h, uint8_t mood, bool leftEye, int hlx, int hly) {
  if (h <= 6) { oled.fillRoundRect(x, cyc - 3, w, 6, 3, SSD1306_WHITE); return; }  // blink / sleepy slit
  int y = cyc - h / 2, r = 20; if (r > h / 2) r = h / 2; if (r > w / 2) r = w / 2;  // very round = cute
  oled.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);
  int cx = x + w / 2, yb = y + h;
  if (mood != EM_SLEEPY) {                            // glossy sparkle: big shine + tiny shine
    oled.fillCircle((leftEye ? cx - 4 : cx + 7) + hlx, y + 12 + hly, 5, SSD1306_BLACK);
    oled.fillCircle(cx + 6 + hlx, yb - 13 + hly, 2, SSD1306_BLACK);
  }
  switch (mood) {
    case EM_HAPPY:                                    // carve the bottom into a smile-squint
      oled.fillCircle(cx, yb + 30, 40, SSD1306_BLACK); break;
    case EM_TIRED:                                    // heavy upper lid -> half closed
      oled.fillRect(x - 1, y - 1, w + 2, (int)(h * 0.44f) + 1, SSD1306_BLACK); break;
    case EM_SAD:                                      // top-outer slant -> inner-up droop
      if (leftEye) oled.fillTriangle(x - 2, y - 2, x + w + 2, y - 2, x - 2, y + h * 3 / 5, SSD1306_BLACK);
      else         oled.fillTriangle(x + w + 2, y - 2, x - 2, y - 2, x + w + 2, y + h * 3 / 5, SSD1306_BLACK);
      break;
    case EM_WORRY:                                    // small top-inner lid -> uneasy
      if (leftEye) oled.fillTriangle(x + w + 2, y - 2, x + w / 3, y - 2, x + w + 2, y + h * 2 / 5, SSD1306_BLACK);
      else         oled.fillTriangle(x - 2, y - 2, x + w - w / 3, y - 2, x - 2, y + h * 2 / 5, SSD1306_BLACK);
      break;
    default: break;                                   // EM_NORMAL — full eye
  }
}

static void drawMonoFace(const String &emoIn, bool offline, uint32_t t) {
  oled.clearDisplay();
  String emo = offline ? String("offline") : emoIn;
  uint8_t mood = eyeMood(emo);
  bool sweat  = (emo == "thirsty" || emo == "hot");
  bool tear   = (emo == "sad");
  bool zzz    = (emo == "sleepy");
  bool shiver = (emo == "cold");

  // pop-bounce when the mood changes
  static String shown = "?"; static uint32_t changeAt = 0;
  if (emo != shown) { shown = emo; changeAt = t; }
  float dtc = (float)(t - changeAt);
  int bnc = (dtc < 420) ? (int)lroundf(expf(-dtc / 140.0f) * sinf(dtc / 40.0f) * 3.0f) : 0;

  // blink — quick each cycle, occasionally a double
  uint32_t bc = t % 3200; bool dbl = ((t / 3200) % 3 == 0); float open = 1.0f;
  auto bw = [](uint32_t d) -> float { return d < 70 ? 1 - d / 70.0f : (d < 140 ? (d - 70) / 70.0f : 1.0f); };
  if (bc < 140) open = bw(bc); else if (dbl && bc >= 220 && bc < 360) open = bw(bc - 220);

  // idle glance
  static float lxf = 0, lyf = 0, ltx = 0, lty = 0; static uint32_t nextLook = 0;
  if (t > nextLook) { nextLook = t + 1100 + ((t / 7) % 2400); uint32_t r = (t / 53) % 6;
    ltx = (r == 0) ? -6 : (r == 1) ? 6 : 0; lty = (r == 3) ? -3 : (r == 4) ? 3 : 0; }
  lxf += (ltx - lxf) * 0.16f; lyf += (lty - lyf) * 0.16f;
  int ox = (int)lroundf(lxf);
  int oy = (int)lroundf(lyf) + bnc + (int)lroundf(sinf(t / 1000.0f));   // glance + pop + breathe
  if (shiver) ox += (int)lroundf(sinf(t / 40.0f) * 2.0f);
  if (emo == "thirsty") oy += 2;                                        // a touch downcast

  // tiny sprout sprig (swaying) in the top-center gap — keeps her a plant
  { int sx = 64 + (int)lroundf(sinf(t / 700.0f) * 1.5f);
    oled.drawFastVLine(sx, 0, 5, SSD1306_WHITE);
    oled.fillTriangle(sx, 5, sx - 5, 0, sx - 1, 0, SSD1306_WHITE);
    oled.fillTriangle(sx, 5, sx + 5, 0, sx + 1, 0, SSD1306_WHITE); }

  const int EW = 44;
  int eh = (int)(42 * open); if (eh < 3) eh = 3;
  if (mood == EM_SLEEPY) eh = 4;
  int eyc = 39 + oy, lx = 8 + ox, rx = 76 + ox;

  // expressive eyebrows above the eyes (follow the glance; don't move on blink)
  drawBrows(lx + EW / 2, rx + EW / 2, eyc - 26, browMode(emo));

  if (mood == EM_HEART) {
    int s = 18 + (sinf(t / 240.0f) > 0.4f ? 2 : 0);            // pulsing hearts
    bigHeart(lx + EW / 2, eyc, s); bigHeart(rx + EW / 2, eyc, s);
  } else {
    int hlx = -(int)lroundf(lxf * 0.3f), hly = -(int)lroundf(lyf * 0.3f);  // shine trails the glance
    int eh2 = (mood == EM_WORRY) ? (int)(eh * 0.82f) : eh;
    drawEye(lx, eyc, EW, eh2, mood, true,  hlx, hly);
    drawEye(rx, eyc, EW, eh2, mood, false, hlx, hly);
  }

  if (sweat) mDrop(120, 10 + (int)((t / 9) % 18));            // dripping sweat
  if (tear)  mDrop(lx + 8, eyc + 18 + (int)((t / 11) % 14));  // falling tear
  if (zzz) { int zb = (int)(sinf(t / 450.0f) * 2); oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1); oled.setCursor(108, 5 + zb); oled.print("z");
    oled.setTextSize(2); oled.setCursor(114, 0); oled.print("Z"); }

  if (offline) { uint32_t s = (t / 300) % 4;                  // "looking for internet"
    for (int k = 0; k < 3; k++) if (s > (uint32_t)(k + 1)) mArc(64, 9, 3 + k * 4, PI * 1.25f, PI * 1.75f);
    oled.fillCircle(64, 9, 1, SSD1306_WHITE); }

  if (mCapText.length() && millis() < mCapUntil) {            // caption on a dark strip
    oled.fillRect(0, 55, 128, 9, SSD1306_BLACK);
    oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
    String s = mCapText.length() > 21 ? mCapText.substring(0, 20) + "." : mCapText;
    int wd = s.length() * 6, xx = (128 - wd) / 2; if (xx < 0) xx = 0;
    oled.setCursor(xx, 56); oled.print(s); }

  oled.display();
}

// ---- display interface implementation (v2 driver) ----
void displaySetup() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);                                      // fast-mode I2C -> smoother animation
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    Serial.println("[DISP] SSD1306 init FAILED (check SDA=D9/SCL=D8/VCC/GND)");
    return;
  }
  mDispReady = true;
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2); oled.setCursor(26, 22); oled.print("Terra");
  oled.display();
  Serial.println("[DISP] SSD1306 128x64 ready (Terra eyes)");
}

void displayRenderFace(const String &emotion, bool offline, uint32_t t) {
  if (!mDispReady) return;
  static uint32_t last = 0;
  if (t - last < 50) return;                                  // ~20 fps, smooth over fast-mode I2C
  last = t;
  drawMonoFace(emotion, offline, t);
}

void displaySetCaption(const String &text, uint32_t ms) { mCapText = text; mCapUntil = millis() + ms; }

// Full-screen two-line message (used to walk a non-techie through WiFi setup).
void displayMessage(const String &line1, const String &line2) {
  if (!mDispReady) return;
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  oled.drawFastVLine(64, 0, 5, SSD1306_WHITE);                    // little sprout
  oled.fillTriangle(64, 5, 59, 0, 63, 0, SSD1306_WHITE);
  oled.fillTriangle(64, 5, 69, 0, 65, 0, SSD1306_WHITE);
  oled.setTextSize(1);
  int w1 = line1.length() * 6, x1 = (128 - w1) / 2; if (x1 < 0) x1 = 0;
  oled.setCursor(x1, 22); oled.print(line1);
  int w2 = line2.length() * 6, x2 = (128 - w2) / 2; if (x2 < 0) x2 = 0;
  oled.setCursor(x2, 40); oled.print(line2);
  oled.display();
}
