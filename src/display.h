// display.h — thin display interface.
// The rest of the app renders Terra's face through ONLY these three calls; the
// concrete panel driver is chosen at compile time by DISPLAY_DRIVER. This keeps
// every panel-specific detail (resolution, color depth, bus, library) behind
// the seam, so swapping panels is a driver drop-in.
#pragma once
#include <Arduino.h>

// DISPLAY_DRIVER:  1 = v1 GC9A01 color round (default)   2 = v2 SSD1306 mono
#ifndef DISPLAY_DRIVER
#define DISPLAY_DRIVER 1
#endif

// ---- the interface (the only display calls the rest of the app makes) ----
void displaySetup();
void displayRenderFace(const String &emotion, bool offline, uint32_t t);
void displaySetCaption(const String &text, uint32_t ms);
void displayMessage(const String &line1, const String &line2);   // e.g. WiFi-setup hint

// ---- bind to the selected concrete driver ----
#if   DISPLAY_DRIVER == 1
  #include "sprout_face.h"   // v1: GC9A01 color round, 240x240, SPI, LovyanGFX
#elif DISPLAY_DRIVER == 2
  #include "mono_face.h"     // v2: SSD1306 128x64 mono, I2C   (added in Phase 2)
#else
  #error "Unknown DISPLAY_DRIVER — use 1 (v1 GC9A01) or 2 (v2 SSD1306)"
#endif
