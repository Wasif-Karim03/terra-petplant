// face_demo — cycle every Terra emotion on the SSD1306 mono OLED, ~2.4s each,
// using the real mono_face.h driver (via display.h with DISPLAY_DRIVER=2).
// The emotion name shows as the bottom caption so you know which is which.
#include <Arduino.h>
#include "display.h"           // -I ../src -> display.h -> mono_face.h

static const char *EMOS[] = {"neutral", "happy", "love", "sleepy",
                             "thirsty", "cold", "hot", "sad"};
static const int N = 8;        // + one "offline" state shown after these

void setup() {
  Serial.begin(115200);
  displaySetup();
  Serial.println("[DEMO] cycling emotions on the OLED (2.4s each)");
}

void loop() {
  static uint32_t sw = 0;
  static int st = -1;
  uint32_t t = millis();
  if (st < 0 || t - sw > 2400) {
    sw = t; st = (st + 1) % (N + 1);          // 0..7 = emotions, 8 = offline
    const char *name = (st < N) ? EMOS[st] : "offline";
    displaySetCaption(name, 2400);
    Serial.printf("[DEMO] %s\n", name);
  }
  bool offline = (st == N);
  displayRenderFace(offline ? "neutral" : EMOS[st], offline, t);
  delay(20);                                   // driver caps redraw at ~15fps
}
