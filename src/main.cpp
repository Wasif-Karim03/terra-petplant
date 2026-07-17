// PetPlant — XIAO ESP32S3
// Pulls real-time clock (NTP), location (IP geolocation), and plant-relevant
// weather (Open-Meteo), and serves it all on a web dashboard for the laptop.
//
//   GET /      -> HTML dashboard (auto-refreshes via the API below)
//   GET /api   -> JSON snapshot of device + time + location + weather
//
// Reach it at  http://<board-ip>/   from any device on the same WiFi.
//
// Data sources (all free, no API key):
//   * Time     : NTP (pool.ntp.org), offset from geolocation
//   * Location : ip-api.com/json  (HTTP)
//   * Weather  : api.open-meteo.com (HTTPS) — temp, humidity, UV, ET0, etc.
//
// NOTE: ADC1 only (A0-A3) — ADC2 conflicts with WiFi on the ESP32.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>   // captive-portal WiFi setup (no hardcoded credentials)
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <DHTesp.h>
#include <math.h>
#include <time.h>
#include <Preferences.h>              // NVS — Aiko's persistent memory
#include <AudioFileSourceLittleFS.h>  // on-device MP3 voice playback
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "display.h"       // thin display interface (driver chosen by DISPLAY_DRIVER)
#include "phrasebank.h"    // generated: buckets + clip filenames (match data/audio/)
#include "plants.h"        // curated plant-care database (thresholds + care guide)

// Set to 1 once the GC9A01 display is physically wired. Kept 0 until then so the
// firmware doesn't waste SPI bandwidth (and slow the web server) drawing to nothing.
#define ENABLE_DISPLAY 0

// ---- WiFi ----
const char *WIFI_SSID = "CaseRegistered";   // open network (no password)
const char *WIFI_PASS = "";

// ---- Units: true = imperial (F, mph, inch), false = metric (C, km/h, mm) ----
const bool USE_IMPERIAL = true;

// ---- Manual location override ----
// Leave USE_MANUAL_LOCATION=false to auto-detect via IP geolocation.
// Set true + fill coords for a precise fixed home location later.
const bool  USE_MANUAL_LOCATION = false;
const float MANUAL_LAT = 41.5043f;   // example: Cleveland, OH
const float MANUAL_LON = -81.6084f;

// ---- Refresh cadence ----
const unsigned long WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL;  // every 10 min
const unsigned long RETRY_INTERVAL_MS   = 60UL * 1000UL;         // retry failures

// On the XIAO ESP32S3 the user LED is on GPIO21, ACTIVE-LOW.
#ifndef LED_BUILTIN
#define LED_BUILTIN 21
#endif

// ---- Sensor pins ----
// Soil sensor analog out -> A0 / D0 (GPIO1, ADC1_CH0, WiFi-safe).
const int SOIL_PIN = A0;
// DHT11 DATA -> D2 (GPIO3). Digital one-wire; not an ADC use.
const int DHT_PIN = 3;
// Light sensor digital DO -> D1 (GPIO2). LOW = light, HIGH = dark.
const int LIGHT_PIN = 2;

// ---- Soil calibration (CAPACITIVE sensor: drier air = HIGHER raw) ----
// Calibrate once: read raw in dry air -> SOIL_AIR_RAW (0%),
// then dunk the probe to the line in water -> SOIL_WATER_RAW (100%).
// These defaults are typical for a capacitive probe on the 12-bit/11dB ADC.
// Bench readings 2025-07: dry-in-air ~2600, in-water ~1190. STILL NEEDS real
// in-soil calibration (dry potting soil vs. just-watered) for accurate %.
int SOIL_AIR_RAW   = 2600;   // 0%   (probe in open air / bone-dry)
int SOIL_WATER_RAW = 1200;   // 100% (probe in water)

DHTesp dht;

WebServer server(80);
unsigned long counter = 0;
int lastSoil = 0;
bool internetOK = false;

// ---- Emotion / expression state (single source of truth for both the
//      web "virtual display" and the future GC9A01 hardware display) ----
// Valid: neutral happy love sleepy thirsty cold hot sad
String emotion = "neutral";
bool   emotionAuto = true;   // false once a face is set manually from the GUI

// ---------------------------------------------------------------------------
// State holders
// ---------------------------------------------------------------------------
struct GeoData {
  bool   valid = false;
  float  lat = 0, lon = 0;
  long   utcOffset = 0;        // seconds
  String city, region, country, timezone;
} geo;

struct WeatherData {
  bool   valid = false;
  unsigned long lastUpdate = 0;
  // current
  float  temp = 0, feels = 0, humidity = 0, precip = 0, wind = 0;
  int    code = -1, isDay = 1, cloud = 0;
  // daily
  float  tMax = 0, tMin = 0, precipSum = 0, uvMax = 0, et0 = 0;
  String sunrise, sunset;
} wx;

bool timeSynced = false;
bool wifiResetPending = false;   // set by the "wifireset" command (dashboard/USB)

// ---------------------------------------------------------------------------
// Local sensor readings (the plant's actual microclimate)
// ---------------------------------------------------------------------------
struct SensorData {
  // DHT11 (air at the plant)
  bool  dhtOK = false;
  float tempC = NAN;      // °C (native DHT11 unit)
  float humidity = NAN;   // %RH
  // Derived from temp + humidity
  float vpd = NAN;        // kPa  (vapor pressure deficit)
  float dewC = NAN;       // °C
  String vpdStatus = "?";
  // Soil
  int   soilRaw = 0;
  float soilPct = NAN;    // 0-100 %
  String soilStatus = "?";
  // Light (digital module: true = light detected, false = dark)
  bool  lightOn = true;
} sx;

// Saturation vapor pressure (kPa) at temperature Tc (°C) — Tetens equation.
static float satVaporPressure(float Tc) {
  return 0.6108f * expf((17.27f * Tc) / (Tc + 237.3f));
}

// VPD: how "thirsty" the air is. The pro watering signal.
//   <0.4 kPa  too humid (fungal/rot risk)
//   0.8-1.2   ideal for most plants
//   >1.6      too dry (transpiration stress)
static void computeVpdAndDew(float Tc, float RH) {
  if (isnan(Tc) || isnan(RH)) { sx.vpd = NAN; sx.dewC = NAN; sx.vpdStatus = "?"; return; }
  float svp = satVaporPressure(Tc);
  sx.vpd = svp * (1.0f - RH / 100.0f);
  float g = logf(RH / 100.0f) + (17.27f * Tc) / (237.3f + Tc);
  sx.dewC = (237.3f * g) / (17.27f - g);
  if      (sx.vpd < 0.4f) sx.vpdStatus = "too humid";
  else if (sx.vpd <= 1.6f) sx.vpdStatus = (sx.vpd >= 0.8f && sx.vpd <= 1.2f) ? "ideal" : "ok";
  else                    sx.vpdStatus = "too dry";
}

// Map raw soil ADC -> 0-100% using the calibration points.
static void computeSoil(int raw) {
  sx.soilRaw = raw;
  float span = (float)(SOIL_AIR_RAW - SOIL_WATER_RAW);
  if (span == 0) { sx.soilPct = NAN; sx.soilStatus = "?"; return; }
  float pct = 100.0f * (SOIL_AIR_RAW - raw) / span;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  sx.soilPct = pct;
  if      (pct < 20.0f) sx.soilStatus = "dry";
  else if (pct < 60.0f) sx.soilStatus = "moist";
  else                  sx.soilStatus = "wet";
}

// Read soil every call; read DHT11 at most once every ~2.5s (its max rate).
void readSensors() {
  computeSoil(analogRead(SOIL_PIN));
  lastSoil = sx.soilRaw;
  sx.lightOn = (digitalRead(LIGHT_PIN) == LOW);   // DO LOW = light present

  static unsigned long lastDht = 0;
  if (millis() - lastDht >= 2500) {
    lastDht = millis();
    TempAndHumidity th = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE && !isnan(th.temperature)) {
      sx.dhtOK = true;
      sx.tempC = th.temperature;
      sx.humidity = th.humidity;
      computeVpdAndDew(sx.tempC, sx.humidity);
    } else {
      sx.dhtOK = false;   // keep last good values; just flag the read failed
    }
  }
}

// Soil sensor is now wired + bench-tested — let real moisture drive thirst.
#define SOIL_CONNECTED 1

// Sleep schedule (local time): quiet/sleepy 22:30 -> 06:30.
#define SLEEP_START_MIN (22*60+30)
#define WAKE_MIN        (6*60+30)

// ---------------------------------------------------------------------------
// Decide the plant's emotion (Auto mode only). HONEST: physical-need moods
// (cold/hot/thirsty) only show when a real sensor backs them. Otherwise it's
// sleepy during sleep hours, happy by day — never faked from outdoor weather.
// ---------------------------------------------------------------------------
void decideEmotion() {
  if (!emotionAuto) return;

  // Sleep window from the device clock.
  bool sleeping = false;
  struct tm tm;
  if (getLocalTime(&tm, 5)) {
    int hm = tm.tm_hour * 60 + tm.tm_min;
    sleeping = (hm >= SLEEP_START_MIN) || (hm < WAKE_MIN);
  }

  // Succulent comfort: fine in heat, likes dry air — only cold + real dryness
  // move the face off "happy" (matches Terra's spoken behaviour).
  String e = "happy";
  if (sleeping) {
    e = "sleepy";
  } else if (sx.dhtOK) {                        // real air sensor present
    if      (sx.tempC < 10)                       e = "cold";   // <50°F — her fear
    else if (sx.tempC > 35)                       e = "hot";    // >95°F — scorch
    else                                          e = "happy";
  }
  if (e == "happy" && SOIL_CONNECTED && !isnan(sx.soilPct) && sx.soilPct < 12)
    e = "thirsty";                              // succulent: only bone-dry reads thirsty

  emotion = e;
}

// ===========================================================================
// WiFi + connectivity
// ===========================================================================
void checkInternet() {
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.begin("http://connectivitycheck.gstatic.com/generate_204");
  int code = http.GET();
  internetOK = (code == 204);
  Serial.printf("[NET] internet=%s (HTTP %d)\n", internetOK ? "yes" : "no", code);
  http.end();
}

// WiFi provisioning via captive portal — no hardcoded credentials. On first
// boot (or if saved WiFi fails) Terra opens an open hotspot "Terra-Setup"; the
// customer connects their phone, the setup page pops up, they pick their home
// WiFi + password, and it's saved to flash. Reachable afterwards at terra.local.
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("terra");

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);       // if setup isn't finished in 3 min, continue offline
  wm.setConnectTimeout(20);
  wm.setClass("invert");                // dark theme for the portal
  wm.setTitle("Terra - Pet Plant Setup");

  Serial.println("[WiFi] connecting (or starting 'Terra-Setup' portal)...");
  bool ok = wm.autoConnect("Terra-Setup");   // blocks during first-time setup only

  if (ok) {
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    Serial.print("[WiFi] Connected. IP: ");
    Serial.print(WiFi.localIP());
    Serial.print("  RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
    if (MDNS.begin("terra")) { MDNS.addService("http", "tcp", 80);
      Serial.println("[mDNS] http://terra.local/"); }
    checkInternet();
  } else {
    Serial.println("[WiFi] setup portal timed out — continuing offline (voice still works).");
  }
}

// Forget saved WiFi and reboot back into setup mode (settings-page button).
void resetWiFi() {
  WiFiManager wm;
  wm.resetSettings();
  delay(300);
  ESP.restart();
}

// ===========================================================================
// Geolocation (ip-api.com, plain HTTP, free, no key)
// ===========================================================================
bool geolocate() {
  if (USE_MANUAL_LOCATION) {
    geo.lat = MANUAL_LAT; geo.lon = MANUAL_LON;
    geo.city = "(manual)"; geo.valid = true;
    Serial.printf("[GEO] manual %.4f, %.4f\n", geo.lat, geo.lon);
    return true;
  }
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.begin("http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,country,timezone,offset");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[GEO] HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err || doc["status"] != "success") {
    Serial.printf("[GEO] parse/status error: %s\n", err.c_str());
    return false;
  }
  geo.lat       = doc["lat"]  | 0.0f;
  geo.lon       = doc["lon"]  | 0.0f;
  geo.utcOffset = doc["offset"] | 0L;
  geo.city      = doc["city"].as<String>();
  geo.region    = doc["regionName"].as<String>();
  geo.country   = doc["country"].as<String>();
  geo.timezone  = doc["timezone"].as<String>();
  geo.valid     = true;
  Serial.printf("[GEO] %s, %s (%.4f, %.4f) tz=%s offset=%lds\n",
                geo.city.c_str(), geo.region.c_str(), geo.lat, geo.lon,
                geo.timezone.c_str(), geo.utcOffset);
  return true;
}

// ===========================================================================
// Time (NTP) — uses the UTC offset from geolocation
// ===========================================================================
bool syncTime() {
  configTime(geo.utcOffset, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  struct tm tm;
  if (!getLocalTime(&tm, 8000)) {
    Serial.println("[TIME] NTP sync failed");
    return false;
  }
  timeSynced = true;
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
  Serial.printf("[TIME] %s\n", b);
  return true;
}

// ===========================================================================
// Weather (Open-Meteo, HTTPS, free, no key)
// ===========================================================================
bool fetchWeather() {
  if (!geo.valid) return false;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += String(geo.lat, 4);
  url += "&longitude=" + String(geo.lon, 4);
  url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
         "precipitation,weather_code,cloud_cover,wind_speed_10m";
  url += "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,uv_index_max,"
         "et0_fao_evapotranspiration,sunrise,sunset";
  url += "&timezone=auto&forecast_days=1";
  if (USE_IMPERIAL)
    url += "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch";

  WiFiClientSecure client;
  client.setInsecure();                 // skip cert validation (fine for public data)
  HTTPClient https;
  https.setConnectTimeout(8000);
  if (!https.begin(client, url)) { Serial.println("[WX] begin failed"); return false; }
  int code = https.GET();
  if (code != 200) {
    Serial.printf("[WX] HTTP %d\n", code);
    https.end();
    return false;
  }

  // Read the full body first, then parse (parsing a TLS stream directly is
  // unreliable with chunked transfer encoding -> InvalidInput).
  String payload = https.getString();
  https.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[WX] parse error: %s (len=%d)\n", err.c_str(), payload.length());
    return false;
  }

  JsonObject cur = doc["current"];
  wx.temp     = cur["temperature_2m"]       | 0.0f;
  wx.feels    = cur["apparent_temperature"] | 0.0f;
  wx.humidity = cur["relative_humidity_2m"] | 0.0f;
  wx.precip   = cur["precipitation"]        | 0.0f;
  wx.wind     = cur["wind_speed_10m"]       | 0.0f;
  wx.code     = cur["weather_code"]         | -1;
  wx.isDay    = cur["is_day"]               | 1;
  wx.cloud    = cur["cloud_cover"]          | 0;

  JsonObject daily = doc["daily"];
  wx.tMax      = daily["temperature_2m_max"][0]            | 0.0f;
  wx.tMin      = daily["temperature_2m_min"][0]            | 0.0f;
  wx.precipSum = daily["precipitation_sum"][0]             | 0.0f;
  wx.uvMax     = daily["uv_index_max"][0]                  | 0.0f;
  wx.et0       = daily["et0_fao_evapotranspiration"][0]    | 0.0f;
  wx.sunrise   = daily["sunrise"][0].as<String>();
  wx.sunset    = daily["sunset"][0].as<String>();

  wx.valid = true;
  wx.lastUpdate = millis();
  Serial.printf("[WX] %.1f%s feels %.1f  hum %.0f%%  UV %.1f  ET0 %.2f  code %d\n",
                wx.temp, USE_IMPERIAL ? "F" : "C", wx.feels, wx.humidity,
                wx.uvMax, wx.et0, wx.code);
  return true;
}

// ===========================================================================
// Web handlers
// ===========================================================================
void handleApi() {
  JsonDocument doc;
  doc["name"]         = "PetPlant";
  doc["uptime_s"]     = millis() / 1000UL;
  doc["heartbeat"]    = counter;
  doc["soil_raw"]     = lastSoil;
  doc["emotion"]      = emotion;
  doc["emotion_auto"] = emotionAuto;

  JsonObject s = doc["sensors"].to<JsonObject>();
  s["dht_ok"]      = sx.dhtOK;
  s["temp_c"]      = isnan(sx.tempC)    ? (float)0 : sx.tempC;
  s["temp_valid"]  = !isnan(sx.tempC);
  s["humidity"]    = isnan(sx.humidity) ? (float)0 : sx.humidity;
  s["vpd_kpa"]     = isnan(sx.vpd)      ? (float)0 : sx.vpd;
  s["vpd_status"]  = sx.vpdStatus;
  s["dewpoint_c"]  = isnan(sx.dewC)     ? (float)0 : sx.dewC;
  s["soil_raw"]    = sx.soilRaw;
  s["soil_pct"]    = isnan(sx.soilPct)  ? (float)0 : sx.soilPct;
  s["soil_status"] = sx.soilStatus;

  JsonObject net = doc["net"].to<JsonObject>();
  net["wifi"]      = (WiFi.status() == WL_CONNECTED);
  net["internet"]  = internetOK;
  net["rssi_dbm"]  = WiFi.RSSI();
  net["ip"]        = WiFi.localIP().toString();
  net["free_heap"] = (unsigned)ESP.getFreeHeap();
  net["free_psram"]= (unsigned)ESP.getFreePsram();

  JsonObject t = doc["time"].to<JsonObject>();
  t["synced"] = timeSynced;
  if (timeSynced) {
    struct tm tm;
    if (getLocalTime(&tm, 50)) {
      char b[40];
      strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
      t["local"] = b;
      t["epoch"] = (uint32_t)time(nullptr);
    }
  }

  JsonObject g = doc["location"].to<JsonObject>();
  g["valid"]    = geo.valid;
  g["city"]     = geo.city;
  g["region"]   = geo.region;
  g["country"]  = geo.country;
  g["timezone"] = geo.timezone;
  g["lat"]      = geo.lat;
  g["lon"]      = geo.lon;

  JsonObject w = doc["weather"].to<JsonObject>();
  w["valid"]      = wx.valid;
  w["units"]      = USE_IMPERIAL ? "imperial" : "metric";
  w["age_s"]      = wx.valid ? (millis() - wx.lastUpdate) / 1000UL : 0;
  w["temp"]       = wx.temp;
  w["feels"]      = wx.feels;
  w["humidity"]   = wx.humidity;
  w["precip"]     = wx.precip;
  w["wind"]       = wx.wind;
  w["code"]       = wx.code;
  w["is_day"]     = wx.isDay;
  w["cloud"]      = wx.cloud;
  w["temp_max"]   = wx.tMax;
  w["temp_min"]   = wx.tMin;
  w["precip_sum"] = wx.precipSum;
  w["uv_max"]     = wx.uvMax;
  w["et0"]        = wx.et0;
  w["sunrise"]    = wx.sunrise;
  w["sunset"]     = wx.sunset;

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

// Set/override the face from the GUI.  /face?set=happy   /face?set=auto
// Whatever is set here drives BOTH the web display and the future GC9A01.
void handleFace() {
  if (server.hasArg("set")) {
    String v = server.arg("set");
    if (v == "auto") {
      emotionAuto = true;
      decideEmotion();
    } else {
      emotionAuto = false;
      emotion = v;            // trusted small set; renderer ignores unknowns
    }
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String out = "{\"emotion\":\"" + emotion + "\",\"emotion_auto\":" +
               (emotionAuto ? "true" : "false") + "}";
  server.send(200, "application/json", out);
}

void handleRoot() {
  static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Terra 🌵</title>
<style>
  :root{color-scheme:dark}*{box-sizing:border-box}
  body{margin:0;min-height:100vh;font-family:ui-monospace,Menlo,monospace;
    background:radial-gradient(1200px 600px at 50% -10%,#16321f,#0c1411 60%);
    color:#d7f5e3;display:flex;flex-direction:column;align-items:center;padding:28px 16px}
  h1{margin:0 0 2px;font-size:1.6rem}.plant{font-size:60px;transition:transform .3s}
  .sub{color:#6fae89;font-size:.8rem;margin-bottom:6px}
  .clock{font-size:1.1rem;color:#cfeee0;margin-bottom:20px}
  .sec{width:100%;max-width:680px;margin-top:14px}
  .sec h2{font-size:.72rem;text-transform:uppercase;letter-spacing:1.5px;color:#6fae89;
    border-bottom:1px solid rgba(120,220,160,.15);padding-bottom:6px;margin:0 0 10px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
  .card{background:rgba(255,255,255,.04);border:1px solid rgba(120,220,160,.15);
    border-radius:12px;padding:13px}
  .label{font-size:.66rem;text-transform:uppercase;letter-spacing:1px;color:#6fae89}
  .value{font-size:1.35rem;font-weight:700;margin-top:5px}
  .ok{color:#4ade80}.bad{color:#f87171}.warn{color:#fbbf24}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  .big{font-size:2.4rem}.foot{margin-top:22px;font-size:.7rem;color:#4d7a61}
  .screen{width:264px;height:264px;border-radius:50%;padding:12px;margin:6px 0 4px;
    background:linear-gradient(145deg,#2a2a2a,#050505);
    box-shadow:0 0 0 2px #3a3a3a,0 12px 34px rgba(0,0,0,.6)}
  .screen canvas{width:240px;height:240px;border-radius:50%;display:block;background:#000}
  .btns{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;max-width:440px;margin-top:12px}
  .btns button{background:rgba(255,255,255,.06);color:#d7f5e3;border:1px solid rgba(120,220,160,.25);
    border-radius:20px;padding:6px 13px;font:inherit;font-size:.74rem;cursor:pointer;text-transform:capitalize}
  .btns button:hover{background:rgba(120,220,160,.15)}
  .btns button.active{background:#34d77f;color:#04210f;font-weight:700;border-color:#34d77f}
  .center{display:flex;flex-direction:column;align-items:center}
  .bubble{max-width:300px;background:#0f241a;border:1px solid rgba(120,220,160,.3);
    border-radius:16px;padding:10px 15px;margin:8px 0 10px;font-size:.9rem;color:#eafff2;
    line-height:1.35;opacity:0;transform:translateY(6px);transition:opacity .3s,transform .3s;position:relative}
  .bubble.show{opacity:1;transform:translateY(0)}
  .bubble:after{content:'';position:absolute;bottom:-9px;left:50%;margin-left:-9px;
    border:9px solid transparent;border-top-color:#0f241a}
  .bubble .aside{display:block;margin-top:6px;font-size:.78rem;color:#9fd9b6;font-style:italic}
  .vctrl{display:flex;gap:8px;align-items:center;justify-content:center;flex-wrap:wrap;margin-top:6px;font-size:.72rem;color:#6fae89}
  .vctrl button{background:rgba(255,255,255,.06);color:#d7f5e3;border:1px solid rgba(120,220,160,.25);
    border-radius:18px;padding:5px 12px;font:inherit;font-size:.73rem;cursor:pointer}
  .vctrl button:hover{background:rgba(120,220,160,.15)}
  .vctrl button.on{background:#34d77f;color:#04210f;font-weight:700;border-color:#34d77f}
</style></head><body>
  <div class="plant" id="plant">🌱</div>
  <h1>Terra</h1>
  <div class="sub"><span class="dot" id="dot"></span><span id="status">connecting…</span>
    · <span id="loc">–</span></div>
  <div class="clock" id="clock">--:--:--</div>

  <div class="sec center"><h2>Plant face — <span id="emoname" style="color:#cfeee0">…</span></h2>
    <div class="bubble" id="bubble"></div>
    <div class="screen"><canvas id="face" width="240" height="240"></canvas></div>
    <div class="label" id="emomode" style="margin-top:4px">mirrors the GC9A01 display</div>
    <div class="vctrl">
      <button id="mutebtn">🔇 Voice off</button>
      <button id="talkbtn">💬 Talk to me</button>
      <button id="waterbtn">💧 Give water</button>
      <button id="musicbtn">🎵 Music off</button>
      <span id="bondlbl"></span>
    </div>
    <div class="btns" id="emobtns"></div>
  </div>

  <div class="sec"><h2>At the plant — sensors <span id="sensorwarn" style="color:#fbbf24"></span></h2>
  <div class="grid">
    <div class="card"><div class="label">Soil moisture</div><div class="value big" id="soilpct">–</div>
      <div class="label" id="soilstat" style="margin-top:4px"></div></div>
    <div class="card"><div class="label">Air temp</div><div class="value" id="stemp">–</div></div>
    <div class="card"><div class="label">Air humidity</div><div class="value" id="shum">–</div></div>
    <div class="card"><div class="label">VPD (air dryness)</div><div class="value" id="vpd">–</div>
      <div class="label" id="vpdstat" style="margin-top:4px"></div></div>
    <div class="card"><div class="label">Dew point</div><div class="value" id="dew">–</div></div>
    <div class="card"><div class="label">Soil (raw ADC)</div><div class="value" id="soilraw">–</div></div>
  </div></div>

  <div class="sec"><h2>Weather <span id="cond" style="color:#cfeee0"></span></h2>
  <div class="grid">
    <div class="card"><div class="label">Now</div><div class="value big" id="temp">–</div></div>
    <div class="card"><div class="label">Feels like</div><div class="value" id="feels">–</div></div>
    <div class="card"><div class="label">Humidity</div><div class="value" id="hum">–</div></div>
    <div class="card"><div class="label">Wind</div><div class="value" id="wind">–</div></div>
    <div class="card"><div class="label">Hi / Lo</div><div class="value" id="hilo">–</div></div>
    <div class="card"><div class="label">UV index (max)</div><div class="value" id="uv">–</div></div>
    <div class="card"><div class="label">ET₀ (water demand)</div><div class="value" id="et0">–</div></div>
    <div class="card"><div class="label">Precip today</div><div class="value" id="precip">–</div></div>
    <div class="card"><div class="label">Sunrise</div><div class="value" id="sr">–</div></div>
    <div class="card"><div class="label">Sunset</div><div class="value" id="ss">–</div></div>
  </div></div>

  <div class="sec"><h2>Device</h2>
  <div class="grid">
    <div class="card"><div class="label">Soil (A0 raw)</div><div class="value" id="soil">–</div></div>
    <div class="card"><div class="label">WiFi RSSI</div><div class="value" id="rssi">–</div></div>
    <div class="card"><div class="label">Internet</div><div class="value" id="net">–</div></div>
    <div class="card"><div class="label">Uptime</div><div class="value" id="up">–</div></div>
    <div class="card"><div class="label">Free heap</div><div class="value" id="heap">–</div></div>
    <div class="card"><div class="label">Free PSRAM</div><div class="value" id="psram">–</div></div>
  </div></div>

  <div class="foot">Polling <code>/api</code> · weather via Open-Meteo · time via NTP</div>
<script>
const WMO={0:"Clear ☀️",1:"Mainly clear 🌤️",2:"Partly cloudy ⛅",3:"Overcast ☁️",
45:"Fog 🌫️",48:"Rime fog 🌫️",51:"Light drizzle 🌦️",53:"Drizzle 🌦️",55:"Heavy drizzle 🌦️",
61:"Light rain 🌧️",63:"Rain 🌧️",65:"Heavy rain 🌧️",71:"Light snow 🌨️",73:"Snow 🌨️",
75:"Heavy snow ❄️",80:"Showers 🌦️",81:"Showers 🌦️",82:"Violent showers ⛈️",
95:"Thunderstorm ⛈️",96:"Storm + hail ⛈️",99:"Storm + hail ⛈️"};
let epoch=0, epochAt=0;
const $=id=>document.getElementById(id);
function tickClock(){ if(!epoch)return; const s=epoch+Math.floor((Date.now()-epochAt)/1000);
  $("clock").textContent=new Date(s*1000).toLocaleString(); }
setInterval(tickClock,1000);

/* ===== "Sprout" — virtual round display (240x240, mirrors the GC9A01) =====
   A cute seedling buddy: tinted body, swaying leaves, big glossy eyes that
   blink + glance around, soft cheeks, per-mood ambient particles. Built from
   simple primitives so the same design ports to LovyanGFX on the GC9A01. */
const EMOS=['happy','love','neutral','sleepy','thirsty','cold','hot','sad'];
const PI=Math.PI;
// body=skin tint, glow=center bg, edge=outer bg, eye/mouth styles, leaf posture
const FACE={
 neutral:{body:'#57c98a',glow:'#173a28',edge:'#081711',eye:'sparkle',mouth:'smile', leaf:'up'},
 happy:  {body:'#5fe39a',glow:'#1e5234',edge:'#0a2417',eye:'happy',  mouth:'grin',  leaf:'up',  blush:1,fx:'sparkle'},
 love:   {body:'#74dba2',glow:'#3c1626',edge:'#180810',eye:'heart',  mouth:'smile', leaf:'up',  blush:2,fx:'hearts'},
 sleepy: {body:'#4d9c87',glow:'#13243d',edge:'#060c18',eye:'sleepy', mouth:'tiny',  leaf:'droop',fx:'stars',dim:.8,zzz:1},
 thirsty:{body:'#c4cf73',glow:'#2c2510',edge:'#161203',eye:'worry',  mouth:'pant',  leaf:'wilt', fx:'sweat'},
 cold:   {body:'#82c6db',glow:'#132f49',edge:'#07182a',eye:'wide',   mouth:'chatter',leaf:'shiver',blush:1,fx:'snow',shiver:1},
 hot:    {body:'#e8a06f',glow:'#3c1808',edge:'#1c0a02',eye:'tired',  mouth:'pant',  leaf:'droop',fx:'heat',sweat:1},
 sad:    {body:'#90a7b9',glow:'#1b1828',edge:'#0c0a14',eye:'sad',    mouth:'frown', leaf:'wilt', fx:'rain',tear:1},
 drinking:{body:'#5fe39a',glow:'#16465f',edge:'#07202c',eye:'happy', mouth:'gulp', leaf:'up', blush:1},
 offline: {body:'#8a9bad',glow:'#1a2230',edge:'#090d15',eye:'worry', mouth:'tiny', leaf:'droop'},
};
let curEmo='neutral', autoMode=true, netOffline=false;   // netOffline: powered but no internet
let caption='', captionUntil=0;                          // tiny on-screen subtitle while talking
function stripEmoji(s){return (s||'').replace(/[\u{1F000}-\u{1FAFF}☀-➿️]/gu,'').replace(/\s+/g,' ').trim();}
// word-wrap text to fit maxW px in <=maxLines lines (ellipsis if longer)
function wrapCap(g,text,maxW,maxLines){const words=text.split(' '),lines=[];let cur='';
  for(const w of words){const t=cur?cur+' '+w:w; if(g.measureText(t).width<=maxW){cur=t;}
    else{ if(cur)lines.push(cur); cur=w; if(lines.length===maxLines-1)break; }}
  if(cur&&lines.length<maxLines)lines.push(cur);
  if(lines.length===maxLines){ // truncate last line with … if text remains
    let last=lines[maxLines-1]; const full=words.join(' ');
    if(g.measureText(lines.join(' ')).width<g.measureText(full).width-2){
      while(last.length&&g.measureText(last+'…').width>maxW)last=last.slice(0,-1); lines[maxLines-1]=last+'…'; }}
  return lines;}
let drinkStart=-9999; const DRINK_MS=5200;   // watering "drinking" animation window
const cv=$("face"), g=cv.getContext('2d');
const rnd=i=>{const x=Math.sin(i*127.1+311.7)*43758.5;return x-Math.floor(x);};

// ---- animation state (persists across frames) ----
let blinkAt=0,nextBlink=900,dbl=false;
let lookCur=[0,0],lookTgt=[0,0],nextLook=1500;
let shownEmo='neutral',changeAt=-9999;

function leaf(x,y,ang,len,col){ g.save();g.translate(x,y);g.rotate(ang);
  g.fillStyle=col;g.beginPath();g.moveTo(0,0);
  g.bezierCurveTo(len*0.42,-len*0.45,len*0.16,-len,0,-len);
  g.bezierCurveTo(-len*0.16,-len,-len*0.42,-len*0.45,0,0);g.fill();
  g.strokeStyle='rgba(255,255,255,.22)';g.lineWidth=1.4;
  g.beginPath();g.moveTo(0,-3);g.lineTo(0,-len*0.82);g.stroke();g.restore(); }
function heart(x,y,s,col){ g.fillStyle=col;g.beginPath();
  g.moveTo(x,y+s*0.75);g.bezierCurveTo(x+s,y-s*0.35,x+s*0.5,y-s,x,y-s*0.25);
  g.bezierCurveTo(x-s*0.5,y-s,x-s,y-s*0.35,x,y+s*0.75);g.fill(); }
function teardrop(x,y,s,col){ g.fillStyle=col;g.beginPath();
  g.moveTo(x,y-s);g.bezierCurveTo(x+s*0.9,y+s*0.1,x+s*0.75,y+s,x,y+s);
  g.bezierCurveTo(x-s*0.75,y+s,x-s*0.9,y+s*0.1,x,y-s);g.fill();
  g.fillStyle='rgba(255,255,255,.5)';g.beginPath();g.arc(x-s*0.25,y,s*0.2,0,7);g.fill(); }

function particles(t,f){
  const k=f.fx; if(!k)return;
  if(k==='stars'){ for(let i=0;i<20;i++){const x=rnd(i)*240,y=rnd(i+7)*150+12,
    a=0.25+0.6*(0.5+0.5*Math.sin(t/500+i*2)),s=1+2*rnd(i+3);
    g.fillStyle='rgba(207,227,255,'+a+')';g.beginPath();g.arc(x,y,s,0,7);g.fill();} }
  else if(k==='snow'){ for(let i=0;i<26;i++){const x=(rnd(i)*240+Math.sin(t/700+i)*12+240)%240,
    y=((t*0.03*(0.5+rnd(i+2))+rnd(i+5)*260))%260,r=1+2.2*rnd(i+1);
    g.fillStyle='rgba(255,255,255,.7)';g.beginPath();g.arc(x,y,r,0,7);g.fill();} }
  else if(k==='hearts'){ for(let i=0;i<11;i++){const prog=((t*0.045*(0.5+rnd(i+1))+rnd(i+4)*260))%270,
    y=240-prog,x=rnd(i)*200+20+Math.sin(t/450+i)*9,a=Math.min(1,prog/40)*Math.min(1,(270-prog)/60);
    g.globalAlpha=0.8*a;heart(x,y,4+4*rnd(i+2),'#ff86ad');g.globalAlpha=1;} }
  else if(k==='sparkle'){ for(let i=0;i<13;i++){const ph=(t/650+rnd(i)*7)%4,sc=Math.max(0,Math.sin(ph*PI/2));
    if(sc<=0)continue;const x=rnd(i+1)*220+10,y=rnd(i+6)*200+15,r=3+5*sc;
    g.fillStyle='rgba(255,243,180,'+(0.9*sc)+')';g.save();g.translate(x,y);
    g.beginPath();for(let j=0;j<4;j++){const a=j*PI/2;g.lineTo(Math.cos(a)*r,Math.sin(a)*r);
      g.lineTo(Math.cos(a+PI/4)*r*0.32,Math.sin(a+PI/4)*r*0.32);}g.closePath();g.fill();g.restore();} }
  else if(k==='rain'){ for(let i=0;i<22;i++){const x=rnd(i)*240,
    y=((t*0.2*(0.6+rnd(i+1))+rnd(i+3)*240))%260;
    g.strokeStyle='rgba(159,200,230,.55)';g.lineWidth=2;g.lineCap='round';
    g.beginPath();g.moveTo(x,y);g.lineTo(x-2,y+9);g.stroke();} }
  else if(k==='heat'){ // sun top-right + rising shimmer
    g.save();g.translate(196,46);g.rotate(t/2600);g.fillStyle='rgba(255,196,90,.5)';
    for(let j=0;j<8;j++){g.rotate(PI/4);g.beginPath();g.moveTo(0,-14);g.lineTo(4,-22);g.lineTo(-4,-22);g.closePath();g.fill();}
    g.restore();g.fillStyle='rgba(255,210,120,.7)';g.beginPath();g.arc(196,46,11,0,7);g.fill();
    for(let i=0;i<5;i++){const x=70+i*25,off=Math.sin(t/300+i)*4;
      g.strokeStyle='rgba(255,170,120,.25)';g.lineWidth=3;g.beginPath();
      g.moveTo(x+off,210);g.quadraticCurveTo(x+off+6,196,x+off,182);g.stroke();} }
}

function drawFace(t){
  const drinking=(t-drinkStart)<DRINK_MS;
  const dprog=drinking?(t-drinkStart)/DRINK_MS:0;             // 0..1 through the drink
  const offline=netOffline&&!drinking;                        // no-internet status (drinking wins)
  const effEmo=drinking?'drinking':(offline?'offline':curEmo);
  const f=FACE[effEmo]||FACE.neutral;
  if(effEmo!==shownEmo){shownEmo=effEmo;changeAt=t;            // pop on change
    if(window.MUSIC&&MUSIC.isOn())MUSIC.setMood(offline?'neutral':effEmo);}  // music follows mood

  // schedule blink + glance
  if(t>nextBlink&&blinkAt===0){blinkAt=t;dbl=rnd(t)<0.28;nextBlink=t+1800+rnd(t+1)*3200;}
  if(t>nextLook){nextLook=t+1100+rnd(t+2)*2600;
    lookTgt=(rnd(t+3)<0.45)?[0,0]:[(rnd(t+4)*2-1)*6,(rnd(t+5)*2-1)*4];}
  lookCur[0]+=(lookTgt[0]-lookCur[0])*0.12;lookCur[1]+=(lookTgt[1]-lookCur[1])*0.12;
  if(offline){lookCur[0]*=0.5;lookCur[1]+=(-5-lookCur[1])*0.2;}  // glance up at the wifi icon
  let open=1;
  if(blinkAt>0){const d=t-blinkAt,bw=x=>x<90?1-x/90:(x<180?(x-90)/90:1);
    if(d<180)open=bw(d);else if(dbl&&d>=240&&d<420)open=bw(d-240);
    if(d>(dbl?420:180))blinkAt=0;}
  if(f.eye==='sleepy')open=Math.min(open,0.32);
  if(f.eye==='tired') open=Math.min(open,0.55);
  if(f.eye==='wide')  open=Math.max(open,1);

  // background
  const bg=g.createRadialGradient(120,108,12,120,120,128);
  bg.addColorStop(0,f.glow);bg.addColorStop(1,f.edge);
  g.fillStyle=bg;g.fillRect(0,0,240,240);
  particles(t,f);

  // character transform: breathe (squash/stretch) + pop bounce + shiver
  const breath=Math.sin(t/850),sy=1+0.04*breath,sx=1-0.03*breath;
  const dt=t-changeAt,pop=1+0.17*Math.exp(-dt/210)*Math.cos(dt/72);
  const shv=f.shiver?Math.sin(t/40)*1.7:0;
  // gulping: body swells on each swallow while pouring (dprog<0.82)
  const gulp=(drinking&&dprog<0.82)?Math.max(0,Math.sin(t/150))*0.08:0;
  const BX=120,BY=142,bw=74,bh=66;
  g.save();g.translate(BX+shv,BY);g.scale((sx-gulp*0.5)*pop,(sy+gulp)*pop);

  // ground shadow
  g.fillStyle='rgba(0,0,0,.18)';g.beginPath();g.ellipse(0,bh-2,bw*0.7,9,0,0,7);g.fill();
  // leaves (behind body top) — posture by mood
  let bend=0,droop=0,jit=0;
  if(f.leaf==='droop'){bend=0.5;} else if(f.leaf==='wilt'){bend=1.0;droop=8;}
  else if(f.leaf==='shiver'){jit=Math.sin(t/45)*0.12;}
  const sway=Math.sin(t/700)*0.1+jit+(drinking?Math.sin(t/110)*0.12:0);
  g.strokeStyle='#3f8f5e';g.lineWidth=5;g.lineCap='round';
  g.beginPath();g.moveTo(0,-bh+8);g.lineTo(0,-bh-6+droop);g.stroke();
  leaf(0,-bh-4+droop,-0.6+bend+sway,26,'#52b878');
  leaf(0,-bh-4+droop, 0.6-bend+sway,26,'#46a86b');

  // body blob with shading + rim light
  g.fillStyle=f.body;g.beginPath();g.ellipse(0,0,bw,bh,0,0,7);g.fill();
  g.fillStyle='rgba(255,255,255,.14)';g.beginPath();g.ellipse(-18,-22,bw*0.55,bh*0.45,-0.3,0,7);g.fill();
  g.fillStyle='rgba(0,0,0,.12)';g.beginPath();g.ellipse(14,26,bw*0.6,bh*0.4,0.2,0,7);g.fill();
  g.strokeStyle='rgba(255,255,255,.25)';g.lineWidth=2;g.beginPath();
  g.ellipse(0,0,bw-1,bh-1,0,PI*1.15,PI*1.85);g.stroke();

  // cheeks
  if(f.blush){const col=f.blush>1?'rgba(255,120,150,.6)':'rgba(255,130,150,.42)';
    [-40,40].forEach(cx=>{const cg=g.createRadialGradient(cx,12,0,cx,12,15);
      cg.addColorStop(0,col);cg.addColorStop(1,'rgba(255,130,150,0)');
      g.fillStyle=cg;g.beginPath();g.arc(cx,12,15,0,7);g.fill();});}

  // ---- eyes ----
  const EYX=27,EYY=-12,lx=lookCur[0],ly=lookCur[1],DK='#1e2b25';
  function ball(ex,ey,kx){const rx=15*kx,ry=Math.max(2.5,18*kx*open);
    if(open<0.12){g.strokeStyle=DK;g.lineWidth=4;g.lineCap='round';
      g.beginPath();g.arc(ex,ey-3,9,0.18*PI,0.82*PI);g.stroke();return;}
    g.fillStyle=DK;g.beginPath();g.ellipse(ex+lx*0.4,ey+ly*0.4,rx,ry,0,0,7);g.fill();
    if(open>0.45){g.fillStyle='rgba(255,255,255,.96)';
      g.beginPath();g.arc(ex-rx*0.32+lx*0.4,ey-ry*0.34+ly*0.4,rx*0.36,0,7);g.fill();
      g.beginPath();g.arc(ex+rx*0.34+lx*0.4,ey+ry*0.28+ly*0.4,rx*0.16,0,7);g.fill();}}
  function arcUp(ex,ey){g.strokeStyle=DK;g.lineWidth=5;g.lineCap='round';
    g.beginPath();g.arc(ex,ey+4,12,PI*1.18,PI*1.82);g.stroke();}
  function brow(ex,ey,dir){g.strokeStyle=DK;g.lineWidth=4;g.lineCap='round';
    g.beginPath();g.moveTo(ex-10*dir,ey-16);g.lineTo(ex+9*dir,ey-9);g.stroke();}
  const s=f.eye;
  if(s==='happy'){arcUp(-EYX,EYY);arcUp(EYX,EYY);}
  else if(s==='heart'){const p=1+0.08*Math.sin(t/200);heart(-EYX+lx*0.4,EYY+ly*0.4,13*p,'#ff5d86');heart(EYX+lx*0.4,EYY+ly*0.4,13*p,'#ff5d86');}
  else{ball(-EYX,EYY,s==='wide'?1.12:1);ball(EYX,EYY,s==='wide'?1.12:1);
    if(s==='sad'){brow(-EYX,EYY-3,-1);brow(EYX,EYY-3,1);}    // inner-up = sad
    if(s==='worry'){brow(-EYX,EYY,-1);brow(EYX,EYY,1);}}

  // ---- mouth ----
  const MY=22;g.strokeStyle=DK;g.lineWidth=4.5;g.lineCap='round';g.lineJoin='round';g.fillStyle=DK;
  const mo=f.mouth;
  if(mo==='smile'){g.beginPath();g.arc(0,MY-6,15,PI*0.18,PI*0.82);g.stroke();}
  else if(mo==='tiny'){g.beginPath();g.arc(0,MY-4,7,PI*0.2,PI*0.8);g.stroke();}
  else if(mo==='frown'){g.beginPath();g.arc(0,MY+14,14,PI*1.2,PI*1.8);g.stroke();}
  else if(mo==='chatter'){g.beginPath();g.moveTo(-13,MY);for(let i=0;i<=4;i++)g.lineTo(-13+i*6.5,MY+(i%2?5:-5));g.stroke();}
  else if(mo==='grin'){g.beginPath();g.moveTo(-15,MY-6);g.quadraticCurveTo(0,MY+14,15,MY-6);g.closePath();g.fill();
    g.fillStyle='#ff7c98';g.beginPath();g.ellipse(0,MY+3,7,4,0,0,PI);g.fill();}
  else if(mo==='pant'){g.beginPath();g.ellipse(0,MY,9,8,0,0,7);g.fill();
    g.fillStyle='#ff8fa3';g.beginPath();g.ellipse(0,MY+5,6,5,0,0,7);g.fill();}
  else if(mo==='gulp'){const op=0.55+0.45*Math.abs(Math.sin(t/150));   // open wide, pulse w/ swallow
    g.beginPath();g.ellipse(0,MY,9,7+9*op,0,0,7);g.fill();
    g.fillStyle='#7ec8ff';g.beginPath();g.ellipse(0,MY+3,6,3+5*op,0,0,7);g.fill();}  // water pooling inside
  g.restore();

  // ---- foreground fx (screen space, near the face) ----
  const fx=BY+EYY;
  if(f.sweat)teardrop(BX+46,fx-8+Math.sin(t/200)*1,5,'#bfe6ff');
  if(f.tear){const fall=(t/9)%26;teardrop(BX-30,fx+14+fall,5,'#9fd8ff');
    if(fall>20)teardrop(BX+30,fx+14+(fall-20),4,'#9fd8ff');}
  if(curEmo==='thirsty')teardrop(BX-46,fx-4+Math.sin(t/240)*1,5,'#8fd0ff');
  if(f.zzz){g.fillStyle='rgba(207,227,255,.9)';const zb=Math.sin(t/450)*2;
    g.font='bold 15px monospace';g.fillText('z',150,72+zb);
    g.font='bold 21px monospace';g.fillText('Z',166,56-zb);}
  if(f.dim){g.fillStyle='rgba(4,6,20,'+(1-f.dim)+')';g.fillRect(0,0,240,240);}

  // ---- offline: searching-wifi icon (rings build up, then a red slash) ----
  if(offline){
    const cx=120, cy=60, cyc=(t/300)%4;          // 0..4 search cycle
    for(let k=0;k<3;k++){ const on=cyc>(k+1);
      g.strokeStyle=on?'#9fd9ff':'rgba(180,200,220,.15)'; g.lineWidth=4; g.lineCap='round';
      g.beginPath(); g.arc(cx,cy,8+k*9,PI*1.25,PI*1.75); g.stroke(); }
    g.fillStyle=cyc>1?'#9fd9ff':'rgba(180,200,220,.3)'; g.beginPath(); g.arc(cx,cy,3.2,0,7); g.fill();
    if(cyc>3){ g.strokeStyle='#ff6b6b'; g.lineWidth=4.5; g.lineCap='round';   // "no signal" slash flash
      g.beginPath(); g.moveTo(cx-20,cy-22); g.lineTo(cx+20,cy+6); g.stroke(); }
    g.fillStyle='rgba(207,227,255,.8)'; g.font='600 12px ui-monospace,monospace'; g.textAlign='center';
    g.fillText('looking for internet…', 120, 214); g.textAlign='left';
  }

  // ---- subtitle while talking: text curved along the bottom rim of the circle ----
  if(caption && t<captionUntil){
    const cx=120,cy=120,R=103;                  // baseline radius near the bottom edge
    g.font='600 12px ui-monospace,monospace'; g.textBaseline='middle'; g.textAlign='center';
    const arcOf=s=>[...s].reduce((a,c)=>a+g.measureText(c).width,0)/R;
    const maxArc=Math.PI*0.95;                   // fit within ~170° of the bottom
    let txt=caption;
    if(arcOf(txt)>maxArc){ while(txt.length&&arcOf(txt+'…')>maxArc)txt=txt.slice(0,-1); txt+='…'; }
    const widths=[...txt].map(c=>g.measureText(c).width), totalArc=widths.reduce((a,b)=>a+b,0)/R;
    const start=Math.PI/2+totalArc/2, end=Math.PI/2-totalArc/2;  // left -> right along bottom
    // place each glyph along the arc, rotated tangent to the circle (clean white, no backing)
    let ang=start;
    g.fillStyle='#ffffff';
    for(let i=0;i<txt.length;i++){ const wA=widths[i]/R; ang-=wA/2;
      const x=cx+Math.cos(ang)*R, y=cy+Math.sin(ang)*R;
      g.save(); g.translate(x,y); g.rotate(ang-Math.PI/2); g.fillText(txt[i],0,0); g.restore();
      ang-=wA/2; }
    g.textAlign='left'; g.textBaseline='alphabetic';
  }

  // ---- drinking water: pouring stream + splash, then satisfied sparkle ----
  if(drinking){
    const mouthY=BY+MY-2;                         // where the stream lands
    if(dprog>0.05 && dprog<0.82){
      const wob=k=>120+Math.sin(k/16+t/80)*3.5;  // gentle wobble down the stream
      g.lineCap='round';
      g.strokeStyle='rgba(110,195,255,.75)';g.lineWidth=8;
      g.beginPath();for(let y=0;y<=mouthY;y+=6){const x=wob(y);y?g.lineTo(x,y):g.moveTo(x,y);}g.stroke();
      g.strokeStyle='rgba(205,238,255,.85)';g.lineWidth=3;
      g.beginPath();for(let y=0;y<=mouthY;y+=6){const x=wob(y);y?g.lineTo(x,y):g.moveTo(x,y);}g.stroke();
      // splash droplets bouncing off the mouth
      for(let i=0;i<6;i++){const ph=(t/430+i*1.27)%1;
        teardrop(120+(i-2.5)*9*ph, mouthY-6+ph*ph*22, 3, 'rgba(191,230,255,'+(1-ph)+')');}
    }
    if(dprog>0.8){                               // "ahh, refreshed!" sparkle burst
      const bp=(dprog-0.8)/0.2;
      for(let i=0;i<9;i++){const a=i/9*PI*2,r=18+bp*46,x=120+Math.cos(a)*r,y=150+Math.sin(a)*r,sc=1-bp,rr=3+4*sc;
        g.fillStyle='rgba(255,243,180,'+sc+')';g.save();g.translate(x,y);
        g.beginPath();for(let j=0;j<4;j++){const aa=j*PI/2;g.lineTo(Math.cos(aa)*rr,Math.sin(aa)*rr);
          g.lineTo(Math.cos(aa+PI/4)*rr*0.3,Math.sin(aa+PI/4)*rr*0.3);}g.closePath();g.fill();g.restore();}
    }
  }
}
function faceLoop(t){ drawFace(t); requestAnimationFrame(faceLoop); }
requestAnimationFrame(faceLoop);

// emotion buttons
const bar=$("emobtns");
['auto',...EMOS].forEach(n=>{ const b=document.createElement('button');
  b.textContent=n; b.dataset.n=n; b.onclick=()=>setFace(n); bar.appendChild(b); });
function markBtns(){ [...bar.children].forEach(b=>{ const n=b.dataset.n;
  b.classList.toggle('active', n==='auto' ? autoMode : (!autoMode && n===curEmo)); }); }
function setFace(n){ fetch('/face?set='+n).catch(()=>{});
  if(n==='auto'){ autoMode=true; } else { autoMode=false; curEmo=n; } markBtns(); }

/* ===== Behaviour config — honesty + daily schedule =====
   SENSE: which physical sensors are actually wired. Until a sensor is present
   Sprout will NOT claim the matching need (no fake thirst/cold/hot). Flip these
   to true (and SOIL_CONNECTED in firmware) once the hardware is connected. */
let SENSE={soil:false, dht:false};
const SCHED={ sleepStart:22*60+30, wake:6*60+30,   // quiet/sleep 22:30 -> 06:30
              morningEnd:11*60, eveningStart:18*60, nightGreet:21*60+30,
              ambientPerDay:2, ambientGapMs:3*3600*1000, minGapMs:20*60*1000 };

/* ===== Sprout's voice — gentle & caring, speaks mainly when it needs something.
   Selector engine + phrasebank (ports to ESP32 C++ + pre-rendered clips later).
   Relationship memory persists in localStorage (stands in for on-device NVS). ===== */
const VOICE=(()=>{
  const TH={soilVeryThirsty:18,soilThirsty:32,soilWet:78,vpdDry:1.6,airColdF:50,airHotF:88,
            wateredJump:18,freshBootMin:5};
  // persistent relationship state
  const S=JSON.parse(localStorage.getItem('petplant')||'{}');
  if(!S.firstSeen)S.firstSeen=Date.now();
  if(!S.celebrated)S.celebrated=[];
  if(S.waterings==null)S.waterings=0;
  const save=()=>localStorage.setItem('petplant',JSON.stringify(S));
  const days=()=>Math.floor((Date.now()-S.firstSeen)/86400000);
  const bond=()=>{const d=days();return d>=14?3:d>=5?2:d>=1?1:0;};

  const recent=[]; const bucketAt={}; let poll=0;
  function map(d){const w=d.weather||{},s=d.sensors||{},t=d.time||{};
    let hour=new Date().getHours(); if(t.local&&t.local.includes(' '))hour=+t.local.split(' ')[1].split(':')[0];
    const cc=w.code, cond=(cc===0||cc===1)?'clear':((cc>=51&&cc<=82)||cc>=95)?'rain':'cloud';
    return {soil_pct:s.soil_pct, dht_ok:s.dht_ok, temp_valid:s.temp_valid,
      air_temp:s.temp_valid?(s.temp_c*9/5+32):null, humidity:s.humidity, vpd:s.vpd_kpa,
      uv:w.uv_max, et0:w.et0, precip:w.precip_sum, wind:w.wind, condition:cond,
      sunset:(w.sunset||'').split('T')[1]||'', hour, uptime_min:Math.floor((d.uptime_s||0)/60)};}
  function derive(d){const c=Object.assign({},d),h=d.hour;
    c.is_morning=h>=6&&h<11; c.is_evening=h>=18&&h<21; c.is_night=h>=21||h<6;
    // HONESTY: a need-state stays 'unknown' unless a real sensor backs it.
    const sp=d.soil_pct, soilOK=SENSE.soil, dhtOK=d.dht_ok===true;
    c.soil_state=(!soilOK||sp==null)?'unknown':sp<TH.soilVeryThirsty?'very_thirsty':sp<TH.soilThirsty?'thirsty':sp>TH.soilWet?'wet':'good';
    const at=dhtOK?d.air_temp:null;
    c.temp_state=at==null?'unknown':at<TH.airColdF?'cold':at>TH.airHotF?'hot':'comfy';
    c.dry_air=dhtOK&&d.vpd!=null&&d.vpd>=TH.vpdDry;
    // only complain about senses if a sensor is expected but failing (not "not installed")
    c.sensor_offline=(SENSE.dht&&d.dht_ok===false);
    c.raining=(d.precip!=null&&d.precip>0.02)||d.condition==='rain';
    c.clear=d.condition==='clear';
    c.fresh_boot=d.uptime_min!=null&&d.uptime_min<TH.freshBootMin;
    c.bond=bond(); c.days=days();
    return c;}

  // phrasebank — gentle, warm, grateful, never naggy. {{name}} -> plant name.
  const NAME=()=>S.name||'Terra';
  // {{owner}} = customer's name (set per unit at generation; "" -> gracefully name-free)
  const B=[
   {id:'pet',prio:120,mood:'love',when:c=>c._pet,lines:["hehe, hi {{owner}}! 🌱","*leans into your hand* …that's nice, {{owner}}.","Oh! Hello {{owner}}! I'm so happy you're here."]},
   {id:'just_watered',prio:110,mood:'love',cd:0,when:c=>c.just_watered,lines:[
     "Ahh… thank you, {{owner}}. That's exactly what I needed.","Mmm, fresh water 🥺 you remembered me, {{owner}}.","*happy little sigh* …perfect. You take such good care of me, {{owner}}."]},
   {id:'milestone',prio:95,mood:'love',when:c=>c._milestone,lines:["{{ms}}"]},
   {id:'very_thirsty',prio:90,mood:'thirsty',when:c=>c.soil_state==='very_thirsty',lines:[
     "{{owner}}… I'm so thirsty. Could I have a little water? please?","My soil's gone bone dry, {{owner}}… I don't feel so good.","A sip of water, maybe? I've been waiting for you, {{owner}}."]},
   {id:'cold',prio:72,mood:'cold',when:c=>c.temp_state==='cold',lines:[
     "Brr… it's chilly, {{owner}}. Could we move somewhere warmer?","I'm a little cold 🥶 my leaves are shivering, {{owner}}.","It's frosty… a cozier spot would be lovely, {{owner}}."]},
   {id:'hot',prio:70,mood:'hot',when:c=>c.temp_state==='hot',lines:[
     "Phew… it's so warm, {{owner}}. A bit of shade, maybe?","I'm getting toasty over here 🥵 some water would help, {{owner}}.","It's hot today, {{owner}}… I'm hanging in there."]},
   {id:'thirsty',prio:64,mood:'thirsty',when:c=>c.soil_state==='thirsty',lines:[
     "My soil's getting dry, {{owner}}… whenever you have a moment.","Starting to feel a little parched, {{owner}} 🌱","A small drink soon would be wonderful, {{owner}}."]},
   {id:'soggy',prio:60,mood:'neutral',when:c=>c.soil_state==='wet'&&!c.just_watered,lines:[
     "Oof, my roots are a bit soggy… maybe let me dry out, {{owner}}?","That's plenty of water for now, thank you {{owner}} 😅","I'm well-watered, {{owner}} — let's give it a rest."]},
   {id:'dry_air',prio:54,mood:'thirsty',when:c=>c.dry_air,lines:[
     "The air feels so dry on my leaves today, {{owner}}.","A little mist would feel nice, {{owner}} — it's parched up here.","My leaves are thirsty even if my soil's okay, {{owner}}."]},
   {id:'intro',prio:130,mood:'love',when:c=>false,lines:[   // first power-up ever (triggered on boot)
     "Oh! Hello, {{owner}}! I'm Terra, your little succulent. I'm so happy to meet you. 🌵",
     "Hi {{owner}}! I'm awake… and you're my person? I think I'm going to love it here."]},
   {id:'welcome',prio:115,mood:'love',when:c=>false,lines:[  // every later power-up / reconnect (triggered on boot)
     "I'm awake! Hi again, {{owner}}. 🌱","Oh — we're back together, {{owner}}! I missed you.",
     "*stretches* …mmm, good to be back. Hello, {{owner}}!","Hello again, {{owner}}! I missed you."]},
   {id:'morning',prio:50,mood:'happy',cd:40,when:c=>c.is_morning,lines:[
     "Good morning, {{owner}}! Did you sleep well?","Morning, {{owner}} 🌱 I love the early light.","A new day together — good morning, {{owner}}."]},
   {id:'evening',prio:48,mood:'sleepy',cd:40,when:c=>c.is_evening,lines:[
     "The light's going soft, {{owner}}… getting a little sleepy.","Evening already, {{owner}}? Today went by gently.","Winding down for the day, {{owner}}… it was a good one."]},
   {id:'night',prio:46,mood:'sleepy',cd:60,when:c=>c.is_night,lines:[
     "*yawn* …goodnight, {{owner}}. I'll be right here.","Shh… it's late. Rest well, {{owner}}. 🌙","Sleepy time… sweet dreams, {{owner}}."]},
   {id:'sensor_offline',prio:38,mood:'neutral',when:c=>c.sensor_offline,lines:[
     "I can't quite feel my surroundings right now, {{owner}}…","My senses are a little fuzzy, {{owner}} — are my wires okay?","I feel a bit disconnected today, {{owner}}."]},
   {id:'content',prio:22,mood:'happy',when:c=>c.soil_state==='good'&&(c.temp_state==='comfy'||c.temp_state==='unknown'),lines:[
     "I feel really good today, {{owner}} 🌱 thank you for that.","Just soaking up the day with you, {{owner}}.","Everything's just right, {{owner}}. I'm a happy little plant."]},
   {id:'idle',prio:10,mood:'neutral',when:c=>true,lines:[
     "Mmm… nice and quiet.","Growing a little every day, you know.","Just being a plant. It's a good life."]},
  ];
  const ASIDE=[
   {when:c=>c.clear&&!c.is_night,lines:["The sun feels lovely today.","What a bright day — I love it."]},
   {when:c=>c.raining,lines:["It's raining out… cozy, isn't it?","I can feel the rain in the air. Nice."]},
   {when:c=>c.is_evening,lines:["The sky's going golden…","Sunset soon — my favorite."]},
   {when:c=>(c.uv!=null&&c.uv>=8&&!c.is_night),lines:["Strong sun today — don't forget your hat!","The UV's high — take care out there."]},
  ];
  function pick(b){const fresh=b.lines.filter(l=>!recent.includes(l));const p=fresh.length?fresh:b.lines;
    return p[Math.floor(Math.random()*p.length)];}
  function milestoneText(c){const ms={1:"{{owner}}, we've made it a whole day together! 🌱",3:"Three days together now, {{owner}} — I'm settling in.",
    7:"A week together, {{owner}} 🥺 you've taken such good care of me.",14:"Two whole weeks, {{owner}}! I feel really at home with you.",
    30:"A month together, {{owner}} 🌳 I've grown so much because of you."};
    const d=c.days; return (ms[d]&&!S.celebrated.includes(d))?{day:d,text:ms[d]}:null;}

  // pure selection (no state mutation) — returns choice or null
  function select(d,extra){
    const c=derive(map(d)); if(extra)Object.assign(c,extra);
    const mil=milestoneText(c); if(mil){c._milestone=true;c._ms=mil;}
    let m=B.filter(b=>b.when(c));
    // cooldown (priority>=80 ignores it)
    let cd=m.filter(b=>{if(b.prio>=80)return true;const cdv=b.cd!=null?b.cd:6;return bucketAt[b.id]==null||(poll-bucketAt[b.id])>cdv;});
    if(cd.length)m=cd;
    const top=Math.max.apply(null,m.map(b=>b.prio));
    const cands=m.filter(b=>b.prio===top);
    const b=cands[Math.floor(Math.random()*cands.length)];
    let text=pick(b); if(b.id==='milestone'&&c._ms)text=c._ms.text;
    // optional caring aside (not for urgent/self buckets)
    let aside=null;
    if(['just_watered','sensor_offline','pet','night'].indexOf(b.id)<0&&Math.random()<0.35){
      const as=ASIDE.filter(a=>a.when(c)); if(as.length){const a=as[Math.floor(Math.random()*as.length)];aside={text:pick(a)};}}
    return {bucket:b.id,mood:b.mood,prio:b.prio,text,aside,ctx:c,_ms:c._ms};}

  function commit(o){poll++;recent.push(o.text);while(recent.length>6)recent.shift();
    bucketAt[o.bucket]=poll; if(o.aside){recent.push(o.aside.text);} if(o._ms){S.celebrated.push(o._ms.day);} save();}

  // detect a real watering (soil jump) — only when the soil sensor is wired
  function observe(d){ if(!SENSE.soil) return false;
    const sp=(d.sensors||{}).soil_pct; let watered=false;
    if(sp!=null&&S.lastSoil!=null&&(sp-S.lastSoil)>=TH.wateredJump){watered=true;S.waterings++;S.lastWatered=Date.now();}
    if(sp!=null)S.lastSoil=sp; save(); return watered;}

  // force a specific bucket (used for scheduled greetings); null if N/A
  function forceBucket(id,d){const c=derive(map(d));const b=B.find(x=>x.id===id);if(!b)return null;
    let text=pick(b); if(id==='milestone'){const m=milestoneText(c);if(!m)return null;text=m.text;c._ms=m;}
    return {bucket:b.id,mood:b.mood,prio:b.prio,text,aside:null,ctx:c,_ms:c._ms};}

  // gentle ambient lines (no physical-state claims — safe without sensors)
  const AMB=["Just happy to share the day with you, {{owner}}.","Growing a little every day, you know.",
    "It's nice having you around, {{owner}}.","Mmm… what a peaceful little moment.","I like it here with you, {{owner}}."];
  function ambientLine(d){const c=derive(map(d));let text;
    const as=ASIDE.filter(a=>a.when(c));
    if(as.length&&Math.random()<0.6){const a=as[Math.floor(Math.random()*as.length)];text=pick(a);}
    else{const fr=AMB.filter(l=>!recent.includes(l));const p=fr.length?fr:AMB;text=p[Math.floor(Math.random()*p.length)];}
    return {bucket:'ambient',mood:'neutral',prio:20,text,aside:null,ctx:c};}

  // power-up welcome: each boot has a stable id = epoch - uptime; greet once per
  // boot, only if caught soon after power-up (up<180s), persisted across reloads.
  if(S.metBefore==null)S.metBefore=false;
  function newBoot(bootEp,up){ if(up>600)return false; return Math.abs(bootEp-(S.lastBootEpoch||0))>5; }
  const markBoot=(bootEp)=>{S.lastBootEpoch=bootEp;save();};
  const isFirstMeeting=()=>!S.metBefore;
  const markMet=()=>{S.metBefore=true;save();};

  // daily once-per-day greeting + ambient-budget tracking
  if(!S.greet)S.greet={}; if(!S.amb)S.amb={day:'',n:0,last:0};
  const greetedToday=(k,date)=>S.greet[k]===date;
  const markGreet=(k,date)=>{S.greet[k]=date;save();};
  function ambientBudget(date,now){ if(S.amb.day!==date)S.amb={day:date,n:0,last:0};
    return S.amb.n<SCHED.ambientPerDay && (now-(S.amb.last||0))>SCHED.ambientGapMs; }
  const markAmbient=(date,now)=>{ if(S.amb.day!==date)S.amb={day:date,n:0,last:0}; S.amb.n++; S.amb.last=now; save(); };

  return {select,commit,observe,days,bond,name:NAME,setName:n=>{S.name=n;save();},
          forceBucket,ambientLine,greetedToday,markGreet,ambientBudget,markAmbient,
          newBoot,markBoot,isFirstMeeting,markMet};
})();

// ---- voice playback (browser TTS preview; real device uses pre-rendered clips) ----
let voiceOn=false, lastAuto=0, _voices=[];
function loadVoices(){_voices=speechSynthesis.getVoices();}
if('speechSynthesis'in window){loadVoices();speechSynthesis.onvoiceschanged=loadVoices;}
function pickVoice(){return _voices.find(v=>/samantha|moira|tessa|karen|fiona|female/i.test(v.name))
    ||_voices.find(v=>v.lang&&v.lang.toLowerCase().startsWith('en'))||_voices[0];}
// silent fallback: browser voice, used only if a pre-rendered clip is missing
function sayTTS(text,vol,force){ if((!voiceOn&&!force)||!('speechSynthesis'in window))return;
  try{speechSynthesis.cancel();}catch(e){}
  const u=new SpeechSynthesisUtterance(text.replace(/[*_~]|🌱|🌙|🥺|🥶|🥵|😅|😊/g,'').trim());
  const v=pickVoice(); if(v)u.voice=v; u.rate=0.9; u.pitch=1.4; u.volume=vol; speechSynthesis.speak(u);}
// normalize + stable id must match the clip generator (gen_voice.py / lineId)
let OWNER_NAME='Tasfia';   // the owner's name woven into spoken lines via {{owner}}
function fillName(t){return t?t.replace(/\{\{owner\}\}/g,OWNER_NAME):t;}
function vnorm(t){return t.replace(/[\u{1F000}-\u{1FAFF}☀-➿️*_~]/gu,'').replace(/\s+/g,' ').trim();}
function lineId(t){let h=5381;t=vnorm(t);for(let i=0;i<t.length;i++)h=((h*33)+t.charCodeAt(i))>>>0;return 'L'+h.toString(36);}
let _curAudio=null;
// Play the pre-rendered cute clip; fall back to the browser voice if it's missing.
function playLine(text,vol,after){
  try{if(_curAudio){_curAudio.pause();}}catch(e){}
  if(window.MUSIC&&MUSIC.isOn())MUSIC.duck(0.25);              // dip music under her voice
  const a=new Audio('/audio/'+lineId(text)+'.mp3'); a.volume=vol; _curAudio=a; let done=false;
  const fin=()=>{if(done)return;done=true;
    if(after){after();} else if(window.MUSIC&&MUSIC.isOn())MUSIC.duck(1);}; // restore when fully done
  a.onended=fin; a.onerror=()=>{sayTTS(text,vol,true);fin();};
  a.play().catch(()=>{sayTTS(text,vol,true);fin();});}
function showBubble(text,aside){const b=$("bubble");
  b.innerHTML=text.replace(/</g,'&lt;')+(aside?('<span class="aside">'+aside.replace(/</g,'&lt;')+'</span>'):'');
  b.classList.add('show');clearTimeout(b._t);b._t=setTimeout(()=>b.classList.remove('show'),8000);}
function present(o,force){const tx=fillName(o.text), ax=o.aside?fillName(o.aside.text):null;
  showBubble(tx,ax);
  // tiny on-screen subtitle (duration scales with length); also tell the device
  const cap=stripEmoji(tx), dur=Math.min(9000,2200+55*cap.length);
  caption=cap; captionUntil=performance.now()+dur;
  fetch('/say?t='+encodeURIComponent(cap)+'&ms='+dur).catch(()=>{});   // physical display caption
  if(voiceOn||force){ playLine(tx,o.ctx.is_night?0.7:1, ax?()=>playLine(ax,0.7):null); }
  $("bondlbl").textContent='· day '+VOICE.days()+' · bond '+'♥'.repeat(Math.max(1,VOICE.bond()));}
// Scheduler: speaks only when there's a real reason, on a daily schedule.
//  watering (anytime) > daily greetings > real sensor needs > a little ambient.
function voiceTick(d){
  // device local time -> minutes-of-day + date string
  const tl=(d.time&&d.time.local)||''; let hm, date='';
  if(tl.includes(' ')){const[da,ck]=tl.split(' ');date=da;const[H,M]=ck.split(':').map(Number);hm=H*60+M;}
  else{const n=new Date();hm=n.getHours()*60+n.getMinutes();}
  const sleep=(hm>=SCHED.sleepStart)||(hm<SCHED.wake);
  if(window.MUSIC) MUSIC.setNight(sleep);            // music goes quiet overnight
  const now=Date.now();

  // 0) power-up welcome — caught shortly after a reboot/reconnect (any hour)
  const ep=(d.time&&d.time.epoch)||0, up=d.uptime_s||0;
  if(d.time&&d.time.synced&&ep>0){
    const bootEp=ep-up;
    if(VOICE.newBoot(bootEp,up)){
      VOICE.markBoot(bootEp);
      const first=VOICE.isFirstMeeting(); if(first)VOICE.markMet();
      const o=VOICE.forceBucket(first?'intro':'welcome',d);
      if(o){ lastAuto=now; VOICE.commit(o); present(o); return; }
    }
  }

  // 1) watering — a direct interaction, acknowledged any time (softer at night)
  const watered=VOICE.observe(d);
  if(watered){ drinkStart=performance.now(); lastAuto=now;
    const o=VOICE.forceBucket('just_watered',d)||VOICE.select(d,{just_watered:true});
    if(o){VOICE.commit(o);present(o);} return; }

  if(sleep) return;                                  // otherwise silent overnight
  if(now-lastAuto<8000) return;                      // debounce vs the 2s poll

  const band = hm<SCHED.morningEnd ? 'morning' : (hm>=SCHED.eveningStart?'evening':'day');
  let o=null, kind=null;

  // 2) daily greetings — once each per day
  if(band==='morning' && !VOICE.greetedToday('morning',date)){ o=VOICE.forceBucket('morning',d); kind='morning'; }
  else if(band==='evening' && hm>=SCHED.nightGreet && !VOICE.greetedToday('night',date)){ o=VOICE.forceBucket('night',d); kind='night'; }

  // 3) real needs + milestone (honesty-gated in derive); never idle/greeting here
  if(!o){ const c=VOICE.select(d);
    if(c && c.prio>=45 && !['idle','morning','evening','night'].includes(c.bucket)){ o=c; kind='need'; } }

  // 4) a little ambient — capped per day, well spaced, only when nothing else
  if(!o && now-lastAuto>SCHED.minGapMs && VOICE.ambientBudget(date,now)){ o=VOICE.ambientLine(d); kind='ambient'; }

  if(!o) return;
  lastAuto=now;
  if(kind==='morning') VOICE.markGreet('morning',date);
  else if(kind==='night') VOICE.markGreet('night',date);
  else if(kind==='ambient') VOICE.markAmbient(date,now);
  VOICE.commit(o); present(o);
}
// controls
function setVoiceBtn(){const b=$("mutebtn");b.textContent=voiceOn?'🔊 Voice on':'🔇 Voice off';b.classList.toggle('on',voiceOn);}
$("mutebtn").onclick=()=>{voiceOn=!voiceOn;setVoiceBtn();
  if(voiceOn) playLine(fillName("Oh — you can hear me now! Hi there, {{owner}}!"),1);
  else { try{speechSynthesis.cancel();}catch(e){} try{if(_curAudio)_curAudio.pause();}catch(e){} } };
$("talkbtn").onclick=()=>{ if(!window._lastApi)return; voiceOn=true; setVoiceBtn();   // a click is a user gesture -> always allowed to speak
  const o=VOICE.select(window._lastApi,{_pet:Math.random()<0.4}); VOICE.commit(o); present(o,true); };
// give water -> drinking animation + grateful line
function giveWater(){ drinkStart=performance.now();
  if(window._lastApi){const o=VOICE.select(window._lastApi,{just_watered:true}); VOICE.commit(o); present(o,true);} }
$("waterbtn").onclick=()=>{ voiceOn=true; setVoiceBtn(); giveWater(); };

/* ===== Mood music — plays pre-rendered seamless loop files from the device
   (/audio/music/music_<mood>.mp3). Crossfades between moods, auto-ducks under
   Sprout's voice. Same files will play on the device speaker later. */
const MUSIC=(()=>{
  const MOODS=['happy','love','neutral','sleepy','thirsty','cold','hot','sad','drinking'];
  const BASE=0.55;                      // master music volume
  let on=false, mood=null, cur=null, duckLvl=1, nightMul=1; const cache={};
  function el(m){ if(!cache[m]){const a=new Audio('/audio/music/music_'+m+'.mp3');a.loop=true;a.volume=0;a.preload='auto';cache[m]=a;} return cache[m]; }
  function fade(a,to,ms){ if(!a)return; const from=a.volume,t0=performance.now();
    if(to>0&&a.paused)a.play().catch(()=>{});
    (function step(){const k=Math.min(1,(performance.now()-t0)/ms);
      a.volume=Math.max(0,Math.min(1,from+(to-from)*k));
      if(k<1)requestAnimationFrame(step); else if(to<=0){try{a.pause();}catch(e){}}})();}
  function target(){ return BASE*duckLvl*nightMul; }
  function setMood(m){ if(!on||MOODS.indexOf(m)<0||m===mood)return; mood=m;
    const nx=el(m), old=cur; cur=nx; fade(nx,target(),1200); if(old&&old!==nx)fade(old,0,1200); }
  function start(){ if(on)return; on=true; }
  function stop(){ on=false; if(cur)fade(cur,0,500); mood=null; cur=null; }
  function duckTo(v){ duckLvl=v; if(cur)fade(cur,target(),140); }
  function setNight(n){ const m=n?0.22:1; if(m===nightMul)return; nightMul=m; if(cur)fade(cur,target(),1500); }
  return {start,stop,setMood,duck:duckTo,setNight,isOn:()=>on};
})();
$("musicbtn").onclick=()=>{const b=$("musicbtn");
  if(MUSIC.isOn()){MUSIC.stop();b.textContent='🎵 Music off';b.classList.remove('on');}
  else{MUSIC.start();MUSIC.setMood(shownEmo||curEmo);b.textContent='🎶 Music on';b.classList.add('on');}};
async function poll(){
  try{
    const d=await (await fetch('/api',{cache:'no-store'})).json();
    const U=d.weather.units==='imperial';
    const T=U?'°F':'°C', W=U?' mph':' km/h', P=U?'"':' mm';
    const c2u=c=>U?(c*9/5+32):c;   // °C -> display unit
    if(d.time&&d.time.epoch){epoch=d.time.epoch;epochAt=Date.now();tickClock();}
    // emotion (authoritative from device)
    if(d.emotion){ curEmo=d.emotion; autoMode=d.emotion_auto;
      $("emoname").textContent=curEmo; markBtns();
      $("emomode").textContent=(autoMode?'AUTO · ':'manual · ')+'mirrors the GC9A01 display'; }
    $("loc").textContent=d.location.valid?(d.location.city+', '+d.location.region):'locating…';
    // sensors (at the plant)
    const s=d.sensors;
    const colorFor=st=>({dry:'bad',wet:'warn','too dry':'bad','too humid':'warn',ideal:'ok',ok:'',moist:'ok'}[st]||'');
    $("soilpct").textContent = isFinite(s.soil_pct)?s.soil_pct.toFixed(0)+'%':'–';
    const ss=$("soilstat");ss.textContent=s.soil_status;ss.className='label '+colorFor(s.soil_status);
    $("soilraw").textContent=s.soil_raw;
    if(s.temp_valid){
      $("stemp").textContent=c2u(s.temp_c).toFixed(1)+T;
      $("shum").textContent=s.humidity.toFixed(0)+'%';
      $("vpd").textContent=s.vpd_kpa.toFixed(2)+' kPa';
      const vs=$("vpdstat");vs.textContent=s.vpd_status;vs.className='label '+colorFor(s.vpd_status);
      $("dew").textContent=c2u(s.dewpoint_c).toFixed(1)+T;
      $("sensorwarn").textContent='';
    }else{
      $("stemp").textContent='–';$("shum").textContent='–';$("vpd").textContent='–';$("dew").textContent='–';
      $("sensorwarn").textContent='· DHT11 not reading (check wiring on D2)';
    }
    // weather
    if(d.weather.valid){
      $("cond").textContent='· '+(WMO[d.weather.code]||('code '+d.weather.code));
      $("temp").textContent=d.weather.temp.toFixed(0)+T;
      $("feels").textContent=d.weather.feels.toFixed(0)+T;
      $("hum").textContent=d.weather.humidity.toFixed(0)+'%';
      $("wind").textContent=d.weather.wind.toFixed(0)+W;
      $("hilo").textContent=d.weather.temp_max.toFixed(0)+' / '+d.weather.temp_min.toFixed(0)+T;
      const uv=$("uv");uv.textContent=d.weather.uv_max.toFixed(1);
      uv.className='value '+(d.weather.uv_max>=8?'bad':d.weather.uv_max>=6?'warn':'');
      $("et0").textContent=d.weather.et0.toFixed(2)+(U?' in':' mm');
      $("precip").textContent=d.weather.precip_sum.toFixed(2)+P;
      $("sr").textContent=(d.weather.sunrise||'').split('T')[1]||'–';
      $("ss").textContent=(d.weather.sunset||'').split('T')[1]||'–';
    } else { $("cond").textContent='· fetching…'; }
    // device
    $("soil").textContent=d.soil_raw;
    $("rssi").textContent=d.net.rssi_dbm+' dBm';
    const net=$("net");net.textContent=d.net.internet?'online':'no route';
    net.className='value '+(d.net.internet?'ok':'bad');
    netOffline = d.net.wifi && !d.net.internet;   // powered + on wifi but no internet -> show offline face
    const u=d.uptime_s,h=(u/3600|0),m=(u%3600/60|0);
    $("up").textContent=(h?h+'h ':'')+m+'m';
    $("heap").textContent=(d.net.free_heap/1024|0)+' KB';
    $("psram").textContent=(d.net.free_psram/1024|0)+' KB';
    $("status").textContent='live';$("dot").style.background='#4ade80';
    window._lastApi=d; voiceTick(d);
    const p=$("plant");p.style.transform='scale(1.08)';setTimeout(()=>p.style.transform='scale(1)',150);
  }catch(e){$("status").textContent='lost connection';$("dot").style.background='#f87171';}
}
poll();setInterval(poll,2000);
</script></body></html>)HTML";
  server.send_P(200, "text/html", PAGE);
}

// ===========================================================================
// ===========================================================================
// ON-DEVICE VOICE ENGINE — the standalone "brain". Reads the real sensors +
// clock, decides what Aiko should say, and plays her MP3 voice clips through
// the MAX98357A speaker. Mirrors the dashboard scheduler: power-up welcome,
// watering thanks, quiet hours, daily greetings, real-sensor needs, ambient.
// Relationship memory persists in NVS (days known, waterings, greetings, boot).
// ===========================================================================
static AudioGeneratorMP3       *vMp3  = nullptr;
static AudioFileSourceLittleFS *vFile = nullptr;
static AudioOutputI2S          *vOut  = nullptr;
static Preferences vPrefs;

// -- user settings (phone-configurable via the settings page, saved in NVS) --
struct Config {
  float volume     = 2.0f;        // audio gain 0..4 (~ "loudness")
  bool  mute       = false;
  int   quietStart = 22 * 60 + 30; // sleep window start (minutes of day)
  int   quietEnd   = 6 * 60 + 30;  // sleep window end
  int   chattiness = 1;            // 0=quiet, 1=normal, 2=chatty
  char  plant[24]  = "";           // selected plant species key (Phase 4)
} cfg;
static Preferences cfgPrefs;
static void cfgLoad() {
  cfgPrefs.begin("cfg", false);
  cfg.volume     = cfgPrefs.getFloat("vol", 2.0f);
  cfg.mute       = cfgPrefs.getBool("mute", false);
  cfg.quietStart = cfgPrefs.getInt("qs", 22 * 60 + 30);
  cfg.quietEnd   = cfgPrefs.getInt("qe", 6 * 60 + 30);
  cfg.chattiness = cfgPrefs.getInt("chat", 1);
  String p = cfgPrefs.getString("plant", "");
  strncpy(cfg.plant, p.c_str(), sizeof(cfg.plant) - 1);
}
static void cfgSave() {
  cfgPrefs.putFloat("vol", cfg.volume); cfgPrefs.putBool("mute", cfg.mute);
  cfgPrefs.putInt("qs", cfg.quietStart); cfgPrefs.putInt("qe", cfg.quietEnd);
  cfgPrefs.putInt("chat", cfg.chattiness); cfgPrefs.putString("plant", cfg.plant);
}
static void cfgApplyVolume() { if (vOut) vOut->SetGain(cfg.mute ? 0.0f : cfg.volume); }
static bool inQuietHours(int hm) {
  int s = cfg.quietStart, e = cfg.quietEnd;
  if (s == e) return false;
  return (s < e) ? (hm >= s && hm < e) : (hm >= s || hm < e);  // handles midnight wrap
}
static uint32_t vFirstSeen=0, vWaterings=0, vLastBootEpoch=0, vAmbLastMs=0, vLastAutoMs=0;
static int  vLastSoilPct=-1, vGreetMorningDay=-1, vGreetEveningDay=-1, vGreetNightDay=-1, vAmbDay=-1, vAmbCount=0;
static bool vMetBefore=false;
static uint32_t vBucketAtMs[PHRASEBANK_N];
// Succulent behaviour trackers -----------------------------------------------
static uint32_t vSoggySinceEp = 0;   // epoch soil first went "wet" (rot timer); 0 = dry
static int      vDimDayStreak = 0;    // consecutive days below the light target
static int      vLightDayKey  = -1;   // yyyymmdd currently accumulating light for
static uint32_t vLightAccumMs = 0;    // bright-light time banked today (RAM)
static uint8_t  vMsDone        = 0;    // milestone bitmask (day 30/14/7/3/1)
static uint32_t vLightEvtMs    = 0;    // last "light came on" moment
static uint32_t vDarkSinceMs   = 0;    // start of continuous daytime darkness
static bool     vCriticalNeed  = false; // set by vPickNeed(): may break quiet hours

// Succulent comfort/danger thresholds. Indoor DHT is °C; outdoor wx is °F when
// USE_IMPERIAL. Terra: craves light, rarely thirsty, fears frost + soggy roots,
// likes dry air (never mist).
static const float    SUC_FROST_C     = 4.4f;    // 40°F — real danger; overrides quiet hours
static const float    SUC_COLD_C      = 10.0f;   // 50°F — uneasy
static const float    SUC_HOT_C       = 35.0f;   // 95°F — scorch worry
static const float    SUC_MUGGY_RH    = 60.0f;   // muggy air
static const int      LIGHT_TARGET_MIN= 360;     // wants ~6 bright hours/day
static const int      DIM_DAYS_CRAVE  = 2;       // dim this many days -> craving light
static const uint32_t SOGGY_SEC       = 3UL*86400; // wet this long -> rot alert
// Watering conversation ------------------------------------------------------
static const uint32_t THIRST_REPEAT_MS = 10UL*60*1000; // re-ask for water every ~10 min until watered
static const uint32_t WATER_GRACE_MS   = 15UL*60*1000; // after a pour, let it soak before talking thirst
static uint32_t vWaterGraceUntil = 0;  // hush thirst while a fresh drink soaks in
static uint32_t vWaterAckMs      = 0;  // last time a watering was acknowledged
static int      vWaterState      = 0;  // 0 = dry/new cycle, 1 = thanked, 2 = told-them-enough

static bool vBusy() { return vMp3 && vMp3->isRunning(); }
static void vPump() { if (vMp3 && vMp3->isRunning()) { if (!vMp3->loop()) vMp3->stop(); } }
static void vPlay(const char *path) {
  if (vMp3->isRunning()) vMp3->stop();
  if (vFile) { delete vFile; vFile = nullptr; }
  vFile = new AudioFileSourceLittleFS(path);
  if (!vMp3->begin(vFile, vOut)) Serial.printf("[VOICE] play FAILED: %s\n", path);
}
static int vBucketIndex(const char *id) {
  for (int i = 0; i < PHRASEBANK_N; i++) if (!strcmp(PHRASEBANK[i].id, id)) return i;
  return -1;
}
static void vSave() {
  vPrefs.putUInt("firstSeen", vFirstSeen); vPrefs.putUInt("waterings", vWaterings);
  vPrefs.putUInt("bootEp", vLastBootEpoch); vPrefs.putBool("met", vMetBefore);
  vPrefs.putInt("lastSoil", vLastSoilPct);
  vPrefs.putInt("gm", vGreetMorningDay); vPrefs.putInt("ge", vGreetEveningDay);
  vPrefs.putInt("gn", vGreetNightDay);
  vPrefs.putUInt("soggy", vSoggySinceEp); vPrefs.putInt("dim", vDimDayStreak);
  vPrefs.putInt("lday", vLightDayKey);    vPrefs.putUChar("ms", vMsDone);
}
// what she's saying right now (for the phone subtitle) — [SFX:...] cues stripped
static String vNowSaying = ""; static uint32_t vSayingAtMs = 0;
static String vClean(const char *s) {
  String r;
  for (const char *p = s; *p; ) {
    if (p[0]=='[' && p[1]=='S' && p[2]=='F' && p[3]=='X') {   // skip a [SFX: ...] cue
      while (*p && *p != ']') p++; if (*p == ']') p++; continue;
    }
    r += *p++;
  }
  r.trim(); return r;
}
static void vSpeak(const char *id) {
  int bi = vBucketIndex(id); if (bi < 0) return;
  const PBBucket *b = &PHRASEBANK[bi];
  const PBLine *ln = &b->lines[random(b->n)];
  Serial.printf("[VOICE] (%s) %s\n", id, ln->text);
  emotion = b->mood;                       // align mood (drives the face too)
  vNowSaying = vClean(ln->text); vSayingAtMs = millis();   // for the phone subtitle
  vPlay(ln->clip);
  vBucketAtMs[bi] = millis(); vLastAutoMs = millis();
}
static int vDayNum() {
  time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
// The plant the customer selected (tunes thresholds); null = generic defaults.
static const Plant *currentPlant() {
  if (cfg.plant[0] == '\0') return nullptr;
  for (int i = 0; i < PLANTS_N; i++) if (!strcmp(PLANTS[i].key, cfg.plant)) return &PLANTS[i];
  return nullptr;
}

// Is it actually daytime where Terra lives? Prefer the weather's real day/night
// flag (from her location's sunrise/sunset); fall back to the clock; else assume
// day. Sun-related lines (craving light, basking, "clear & sunny") only make
// sense while the sun is actually up.
static bool vIsDaytime() {
  if (wx.valid) return wx.isDay != 0;
  struct tm tm;
  if (getLocalTime(&tm, 5)) return tm.tm_hour >= 7 && tm.tm_hour < 19;
  return true;
}

// Which need is active, in Terra's succulent priority order. Honest — every
// branch is backed by a real sensor. Sets vCriticalNeed when it's the one thing
// that can break quiet hours (frost). Soil thresholds come from the selected
// plant, else succulent defaults (dry ~20%, wet ~55%).
static const char *vPickNeed() {
  vCriticalNeed = false;
  const Plant *pl = currentPlant();
  int dryPct = pl ? pl->soilDry : 20;
  int wetPct = pl ? pl->soilWet : 55;

  // 1) frost / cold — the succulent's one true fear
  if (sx.dhtOK && sx.tempC < SUC_COLD_C) {
    if (sx.tempC < SUC_FROST_C) vCriticalNeed = true;   // near-freezing: speak anytime
    return "cold";
  }
  // 2) overwatering / rot — soil has stayed wet too long (the real danger)
  if (SOIL_CONNECTED && !isnan(sx.soilPct) && sx.soilPct > wetPct && vSoggySinceEp) {
    time_t ep = time(nullptr);
    if (ep > 1000000000L && (uint32_t)ep - vSoggySinceEp > SOGGY_SEC) return "overwater";
  }
  // 3) genuine thirst (rare) — bone dry
  if (SOIL_CONNECTED && !isnan(sx.soilPct) && sx.soilPct < dryPct * 0.5f) return "thirst_l2";
  // 4) heat / leaf scorch
  if (sx.dhtOK && sx.tempC > SUC_HOT_C) return "hot";
  // 5) craving light — dim for several days (only worth asking while sun is up)
  if (vDimDayStreak >= DIM_DAYS_CRAVE && vIsDaytime()) return "craving_light";
  // 6) mild thirst — no rush
  if (SOIL_CONNECTED && !isnan(sx.soilPct) && sx.soilPct < dryPct) return "thirst_l1";
  // 7) muggy air — she likes it dry (never suggests misting)
  if (sx.dhtOK && !isnan(sx.humidity) && sx.humidity > SUC_MUGGY_RH) return "humid";
  // 8) air sensor not answering — "my senses are fuzzy"
  if (!sx.dhtOK) return "senses_fuzzy";
  return nullptr;
}

// Map the live outdoor weather (Open-Meteo WMO code + temp/wind) to a weather
// bucket, so Terra can react to real rain/snow/etc. with no physical sensor.
static const char *vPickWeather() {
  if (!wx.valid || wx.code < 0) return nullptr;
  int   c    = wx.code;
  float tF   = USE_IMPERIAL ? wx.temp : wx.temp * 9.0f / 5.0f + 32.0f;
  float wmph = USE_IMPERIAL ? wx.wind : wx.wind * 0.621371f;
  bool day = vIsDaytime();
  // These read the same day or night:
  if (c >= 95)                              return "wx_storm";   // thunderstorm
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "wx_snow";
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return "wx_rain";
  if (wmph >= 25)                           return "wx_windy";
  if (c == 45 || c == 48 || wx.cloud > 70)  return "wx_cloudy";
  // Sun/brightness lines only make sense in daylight:
  if (day && tF >= 95)                      return "wx_heatwave";
  if (day && (c == 0 || c == 1) && tF >= 70 && tF <= 90 && wx.humidity < 45) return "wx_warmdry";
  if (day && c <= 2)                        return "wx_clear";
  return day ? "wx_cloudy" : nullptr;       // at night, no "sunny" remark — let her muse instead
}

// A little ambient chatter — favours what's true right now (basking in good
// light, the weather outside, dry air she likes) then falls back to musing.
static const char *vPickAmbient() {
  const char *wxb = vPickWeather();
  int  r      = random(100);
  bool bright = sx.lightOn && vIsDaytime();   // basking needs real sun, not a night lamp
  bool comfy  = sx.dhtOK && sx.tempC >= 12.8f && sx.tempC <= 26.7f;
  bool dryair = sx.dhtOK && !isnan(sx.humidity) && sx.humidity < 45.0f;
  if (bright && comfy && r < 25) return "basking";
  if (wxb && r < 60)             return wxb;
  if (dryair && r < 75)          return "dry_content";
  return random(2) ? "content" : "musing";
}

// Book-keeping run every loop: the rot timer, the daily light budget (-> dim-day
// streak that drives craving_light), and continuous daytime darkness.
static void vTrackClimate(time_t nowEp, bool synced, struct tm &tm) {
  uint32_t nowMs = millis();
  static uint32_t prevMs = 0;
  uint32_t dt = prevMs ? (nowMs - prevMs) : 0; prevMs = nowMs;

  // rot timer: mark when soil first goes wet, clear the moment it dries
  if (SOIL_CONNECTED && !isnan(sx.soilPct)) {
    const Plant *pl = currentPlant(); int wetPct = pl ? pl->soilWet : 55;
    if (sx.soilPct > wetPct) { if (!vSoggySinceEp && nowEp > 1000000000L) vSoggySinceEp = (uint32_t)nowEp; }
    else vSoggySinceEp = 0;
  }
  if (!synced) return;

  int day = vDayNum(), hh = tm.tm_hour;
  if (vLightDayKey != day) {                 // new day: judge yesterday's light
    if (vLightDayKey != -1) {
      if ((int)(vLightAccumMs / 60000) < LIGHT_TARGET_MIN) vDimDayStreak++;
      else vDimDayStreak = 0;
      vSave();
    }
    vLightDayKey = day; vLightAccumMs = 0;
  }
  bool daytime = vIsDaytime();               // real sun (from weather) when we have it
  if (sx.lightOn && daytime) vLightAccumMs += dt;   // only real daylight counts as "sun"

  if (daytime && !sx.lightOn) { if (!vDarkSinceMs) vDarkSinceMs = nowMs; }  // dark in daytime
  else vDarkSinceMs = 0;
  (void)hh;
}

void voiceBegin() {
  vOut = new AudioOutputI2S();
  vOut->SetPinout(5, 4, 6);                // BCLK=D4(GPIO5), LRC=D3(GPIO4), DIN=D5(GPIO6)
  cfgLoad();
  cfgApplyVolume();                        // sets gain from saved volume/mute
  vMp3 = new AudioGeneratorMP3();
  vPrefs.begin("terra", false);
  vFirstSeen = vPrefs.getUInt("firstSeen", 0); vWaterings = vPrefs.getUInt("waterings", 0);
  vLastBootEpoch = vPrefs.getUInt("bootEp", 0); vMetBefore = vPrefs.getBool("met", false);
  vLastSoilPct = vPrefs.getInt("lastSoil", -1);
  vGreetMorningDay = vPrefs.getInt("gm", -1); vGreetEveningDay = vPrefs.getInt("ge", -1);
  vGreetNightDay = vPrefs.getInt("gn", -1);
  vSoggySinceEp = vPrefs.getUInt("soggy", 0); vDimDayStreak = vPrefs.getInt("dim", 0);
  vLightDayKey  = vPrefs.getInt("lday", -1);  vMsDone = vPrefs.getUChar("ms", 0);
  for (int i = 0; i < PHRASEBANK_N; i++) vBucketAtMs[i] = 0;
  Serial.printf("[VOICE] engine ready (met=%d, waterings=%u)\n", vMetBefore, vWaterings);
}

void voiceLoop() {
  vPump();
  if (vBusy()) return;                     // never interrupt a clip

  uint32_t nowMs = millis(), upSec = nowMs / 1000;
  // Clock is OPTIONAL — only the day/night schedule needs it. Everything else
  // (welcome, watering, needs) works offline so she's never mute without WiFi.
  struct tm tm; bool haveClock = getLocalTime(&tm, 5);
  time_t nowEp = time(nullptr);
  bool synced = haveClock && nowEp > 1000000000L;
  int hm = synced ? (tm.tm_hour * 60 + tm.tm_min) : -1;
  bool sleeping = synced && inQuietHours(hm);
  if (synced && vFirstSeen == 0) { vFirstSeen = (uint32_t)nowEp; vSave(); }
  // chattiness tuning: 0 quiet, 1 normal, 2 chatty
  uint32_t needCd  = cfg.chattiness == 0 ? 4UL*3600*1000 : cfg.chattiness == 2 ? 1UL*3600*1000 : 2UL*3600*1000;
  int      ambMax  = cfg.chattiness == 0 ? 0 : cfg.chattiness == 2 ? 4 : 2;
  uint32_t ambGap  = cfg.chattiness == 2 ? 90UL*60*1000 : 3UL*3600*1000;
  uint32_t minGap  = cfg.chattiness == 2 ? 10UL*60*1000 : 20UL*60*1000;

  vTrackClimate(nowEp, synced, tm);         // rot timer, light budget, darkness
  const char *need = vPickNeed();            // also sets vCriticalNeed

  // 0) power-up greeting — once per boot (RAM flag), NO clock needed. After a
  //    long absence (>3 days since last boot) she notices you were gone.
  static bool bootSpoken = false;
  if (!bootSpoken && upSec > 3) {
    bootSpoken = true;
    bool first = !vMetBefore; vMetBefore = true;
    bool longGone = !first && vLastBootEpoch && nowEp > 1000000000L &&
                    (uint32_t)nowEp - vLastBootEpoch > 3UL * 86400;
    if (nowEp > 1000000000L) vLastBootEpoch = (uint32_t)nowEp;
    vSave();
    vSpeak(first ? "intro" : (longGone ? "welcome_back_long" : "welcome"));
    return;
  }

  // 1) watering — detect a pour (soil jump). Thank once per drying cycle; if they
  //    keep pouring past "wet", gently say that's enough. Each pour opens a grace
  //    window so the drink can soak in before she talks about thirst again.
  if (SOIL_CONNECTED && !isnan(sx.soilPct)) {
    int sp = (int)sx.soilPct;
    const Plant *pl = currentPlant();
    int wetPct = pl ? pl->soilWet : 55;
    int dryPct = pl ? pl->soilDry : 20;
    int prev = vLastSoilPct; vLastSoilPct = sp;
    // once she's dried out again (and it's been a while), it's a fresh cycle
    if (sp < dryPct && (vWaterAckMs == 0 || nowMs - vWaterAckMs > 30UL * 60 * 1000)) vWaterState = 0;
    if (prev >= 0 && sp - prev >= 12) {                     // a pour just happened
      vSoggySinceEp = 0; vWaterGraceUntil = nowMs + WATER_GRACE_MS; vWaterAckMs = nowMs;
      if (sp > wetPct && vWaterState < 2) {                 // amply watered / they kept pouring
        vWaterState = 2; vSave(); vSpeak("overwater"); return;
      }
      if (vWaterState == 0) {                               // first drink of this cycle -> thanks
        vWaterState = 1; vWaterings++; vSave(); vSpeak("just_watered"); return;
      }
      // already acknowledged this cycle -> stay quiet, let it soak
    }
  }

  // 2) a CRITICAL need (frost) may speak even during quiet hours, and repeats on
  //    the short interval so it's heard even if no one is in the room.
  if (need && vCriticalNeed) {
    int bi = vBucketIndex(need);
    if (bi >= 0 && (vBucketAtMs[bi] == 0 || nowMs - vBucketAtMs[bi] > THIRST_REPEAT_MS)) { vSpeak(need); return; }
  }

  if (sleeping) return;                     // otherwise silent overnight
  if (nowMs - vLastAutoMs < 8000) return;   // debounce

  // 3) milestones — day 1 / 3 / 7 / 14 / 30 together (once each)
  if (synced && vFirstSeen) {
    int days = ((uint32_t)nowEp - vFirstSeen) / 86400;
    static const int MS[] = {30, 14, 7, 3, 1};
    for (int k = 0; k < 5; k++) {
      if (days >= MS[k] && !(vMsDone & (1 << k))) {
        vMsDone |= (1 << k); vSave();
        char id[16]; snprintf(id, sizeof(id), "milestone_%d", MS[k]);
        vSpeak(id); return;
      }
    }
  }

  // 4) daily greetings — morning / evening / night, once each (needs the clock)
  if (synced) {
    int day = vDayNum();
    if (hm < 11 * 60) {
      if (vGreetMorningDay != day) { vGreetMorningDay = day; vSave(); vSpeak("morning"); return; }
    } else if (hm >= 21 * 60 + 30) {
      if (vGreetNightDay != day)   { vGreetNightDay = day; vSave(); vSpeak("night"); return; }
    } else if (hm >= 18 * 60) {
      if (vGreetEveningDay != day) { vGreetEveningDay = day; vSave(); vSpeak("evening"); return; }
    }
  }

  // 5) the light just came on in the evening — "you're home"
  if (synced) {
    static bool prevLight = true;
    if (sx.lightOn && !prevLight && (hm >= 18 * 60 || hm < 6 * 60)) {
      if (vLightEvtMs == 0 || nowMs - vLightEvtMs > 30UL * 60 * 1000) {
        vLightEvtMs = nowMs; prevLight = sx.lightOn;
        vSpeak((hm >= 23 * 60 || hm < 6 * 60) ? "light_late" : "light_on"); return;
      }
    }
    prevLight = sx.lightOn;
  }

  // 6) it's been dark a while in the daytime (mild, gentle)
  if (synced && vDarkSinceMs && nowMs - vDarkSinceMs > 2UL * 3600 * 1000) {
    int bi = vBucketIndex("dark_long");
    if (bi >= 0 && (vBucketAtMs[bi] == 0 || nowMs - vBucketAtMs[bi] > needCd)) { vSpeak("dark_long"); return; }
  }

  // 7) sensor-backed needs. Water (and cold) are calls to action: they repeat
  //    every ~10 min until resolved, in case no one was in the room. A fresh
  //    drink is given time to soak in (grace) before she asks about water again.
  if (need) {
    int  bi       = vBucketIndex(need);
    bool isThirst = !strcmp(need, "thirst_l1") || !strcmp(need, "thirst_l2");
    bool actNow   = isThirst || !strcmp(need, "cold");   // needs a human to do something
    bool soaking  = isThirst && nowMs < vWaterGraceUntil;
    uint32_t cd   = actNow ? THIRST_REPEAT_MS : needCd;
    if (!soaking && bi >= 0 && (vBucketAtMs[bi] == 0 || nowMs - vBucketAtMs[bi] > cd)) { vSpeak(need); return; }
  }

  // 8) ambient — weather-aware, basking, content; capped per day, spaced out
  if (ambMax > 0 && nowMs - vLastAutoMs > minGap) {
    int day = synced ? vDayNum() : 0;
    if (vAmbDay != day) { vAmbDay = day; vAmbCount = 0; }
    if (vAmbCount < ambMax && (vAmbLastMs == 0 || nowMs - vAmbLastMs > ambGap)) {
      vAmbCount++; vAmbLastMs = nowMs; vSpeak(vPickAmbient()); return;
    }
  }
}

// ===========================================================================
// Settings page (the customer's phone control panel at aiko.local).
// ===========================================================================
void handleSettings() {
  static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Terra</title><style>
 :root{color-scheme:dark}*{box-sizing:border-box}
 body{margin:0;font-family:ui-rounded,-apple-system,Segoe UI,Roboto,sans-serif;
   background:radial-gradient(1000px 500px at 50% -10%,#16321f,#0b1310 60%);color:#e6fff0;
   min-height:100vh;padding:22px 16px 40px;display:flex;flex-direction:column;align-items:center}
 .wrap{width:100%;max-width:460px}
 h1{font-size:1.7rem;margin:2px 0;text-align:center}
 .sub{text-align:center;color:#6fae89;font-size:.8rem;margin-bottom:18px}
 .card{background:rgba(255,255,255,.05);border:1px solid rgba(120,220,160,.15);
   border-radius:16px;padding:16px;margin-bottom:14px}
 .card h2{font-size:.72rem;letter-spacing:1.5px;text-transform:uppercase;color:#6fae89;margin:0 0 12px}
 .row{display:flex;justify-content:space-between;align-items:center;gap:10px;margin:10px 0}
 .live{display:grid;grid-template-columns:1fr 1fr;gap:10px}
 .live div .v{font-size:1.3rem;font-weight:700}.live div .k{font-size:.66rem;color:#6fae89;text-transform:uppercase}
 input[type=range]{width:100%;accent-color:#34d77f}
 input[type=time]{background:#0f241a;color:#e6fff0;border:1px solid rgba(120,220,160,.25);border-radius:8px;padding:6px 8px;font:inherit}
 .seg{display:flex;gap:6px}.seg button{flex:1;background:rgba(255,255,255,.06);color:#d7f5e3;
   border:1px solid rgba(120,220,160,.25);border-radius:20px;padding:8px;font:inherit;font-size:.8rem;cursor:pointer}
 .seg button.on{background:#34d77f;color:#04210f;font-weight:700;border-color:#34d77f}
 .toggle{width:52px;height:30px;border-radius:20px;background:#33413a;position:relative;cursor:pointer;transition:.2s;border:none}
 .toggle.on{background:#34d77f}.toggle:after{content:'';position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}
 .toggle.on:after{left:25px}
 .danger{width:100%;background:#3a1620;color:#ff9fb0;border:1px solid #a44;border-radius:12px;padding:12px;font:inherit;cursor:pointer}
 .plant-note{font-size:.8rem;color:#9fd9b6;margin-top:6px}
 a.dash{color:#6fae89;font-size:.75rem;text-decoration:none}
</style></head><body><div class="wrap">
 <h1>🌵 Terra</h1><div class="sub"><span id="dot">●</span> <span id="stat">connecting…</span></div>

 <div class="card"><h2>Live sensors — at the plant</h2>
  <div class="live">
   <div><div class="k">🌱 Soil moisture</div><div class="v" id="soil">–</div><div class="k" id="soilst"></div></div>
   <div><div class="k">🌡️ Temperature</div><div class="v" id="temp">–</div></div>
   <div><div class="k">💧 Humidity</div><div class="v" id="hum">–</div></div>
   <div><div class="k">🏜️ Air dryness</div><div class="v" id="vpd">–</div><div class="k" id="vpdst"></div></div>
   <div><div class="k">💦 Dew point</div><div class="v" id="dew">–</div></div>
   <div><div class="k">💡 Light</div><div class="v" id="light">–</div></div>
  </div></div>

 <div class="card"><h2>Status</h2>
  <div class="live">
   <div><div class="k">😊 Mood</div><div class="v" id="mood">–</div></div>
   <div><div class="k">📶 WiFi</div><div class="v" id="wifi">–</div></div>
   <div><div class="k">🚿 Times watered</div><div class="v" id="wat">–</div></div>
   <div><div class="k">⏱️ Uptime</div><div class="v" id="up">–</div></div>
   <div><div class="k">🌤️ Outside</div><div class="v" id="out">–</div></div>
   <div><div class="k">☀️ UV index</div><div class="v" id="uv">–</div></div>
  </div></div>

 <div class="card"><h2>Voice</h2>
  <div class="row"><span>Volume</span><span id="vollbl" style="color:#6fae89">–</span></div>
  <input type="range" id="vol" min="0" max="100" step="5">
  <div class="row"><span>Mute</span><button class="toggle" id="mute"></button></div>
 </div>

 <div class="card"><h2>Chattiness</h2>
  <div class="seg" id="chat">
   <button data-v="0">Quiet</button><button data-v="1">Normal</button><button data-v="2">Chatty</button>
  </div></div>

 <div class="card"><h2>Quiet hours (she sleeps)</h2>
  <div class="row"><span>From</span><input type="time" id="qs"></div>
  <div class="row"><span>Until</span><input type="time" id="qe"></div>
 </div>

 <div class="card"><h2>My plant</h2>
  <select id="plant" style="width:100%;background:#0f241a;color:#e6fff0;border:1px solid rgba(120,220,160,.25);border-radius:10px;padding:10px;font:inherit"></select>
  <div id="care" style="margin-top:12px;display:none">
   <div class="row"><span>☀️ Light</span><span id="c_light" style="color:#cfeee0;text-align:right;max-width:60%"></span></div>
   <div class="row"><span>💧 Water</span><span id="c_water" style="color:#cfeee0;text-align:right;max-width:60%"></span></div>
   <div class="row"><span>🌡️ Comfort</span><span id="c_temp" style="color:#cfeee0"></span></div>
   <div class="plant-note" id="c_care"></div>
   <div class="plant-note" style="color:#6fae89;margin-top:8px">Terra now watches this plant's ideal range for you.</div>
  </div>
 </div>

 <div class="card"><h2>Network</h2>
  <button class="danger" id="wifi">Change / forget WiFi</button>
  <div class="row" style="justify-content:center;margin-top:10px"><a class="dash" href="/dashboard">developer dashboard →</a></div>
 </div>
</div>
<script>
const $=id=>document.getElementById(id);
let C={};
function m2t(m){return String(m/60|0).padStart(2,'0')+':'+String(m%60).padStart(2,'0');}
function t2m(t){const[a,b]=t.split(':').map(Number);return a*60+b;}
async function save(k,v){await fetch('/cfg?'+k+'='+encodeURIComponent(v)).catch(()=>{});}
function fill(){
  $('vol').value=Math.round(C.volume/4*100); $('vollbl').textContent=Math.round(C.volume/4*100)+'%';
  $('mute').classList.toggle('on',C.mute);
  [...$('chat').children].forEach(b=>b.classList.toggle('on',+b.dataset.v===C.chattiness));
  $('qs').value=m2t(C.quietStart); $('qe').value=m2t(C.quietEnd);
}
function fmtUp(s){s=s||0;const h=s/3600|0,m=s%3600/60|0;return (h?h+'h ':'')+m+'m';}
function live(){
  const set=(id,v)=>{const e=$(id); if(e)e.textContent=v;};
  set('soil', isFinite(C.soil_pct)?Math.round(C.soil_pct)+'%':'–');
  set('soilst', (C.soil_status||'')+(C.soil_raw?' · raw '+C.soil_raw:''));
  set('temp', C.dht_ok?C.temp_c.toFixed(1)+'°C':'no sensor');
  set('hum',  C.dht_ok?Math.round(C.humidity)+'%':'–');
  set('vpd',  C.dht_ok?C.vpd.toFixed(2)+' kPa':'–');
  set('vpdst',C.dht_ok?(C.vpd_status||''):'');
  set('dew',  C.dht_ok?C.dewpoint.toFixed(1)+'°C':'–');
  set('light',C.light?'☀️ Bright':'🌙 Dark');
  set('mood', C.mood||'–');
  set('wifi', C.wifi?(C.rssi+' dBm'):'offline');
  set('wat',  C.waterings);
  set('up',   fmtUp(C.uptime_s));
  set('out',  C.wx_valid?(C.out_temp_c.toFixed(0)+'°C · '+Math.round(C.out_hum)+'%'):'–');
  set('uv',   C.wx_valid?C.uv.toFixed(1):'–');
  $('stat').textContent='live'; $('dot').style.color='#4ade80';
}
async function poll(){try{C=await(await fetch('/cfg',{cache:'no-store'})).json();live();}catch(e){$('stat').textContent='lost connection';$('dot').style.color='#f87171';}}
$('vol').oninput=e=>{$('vollbl').textContent=e.target.value+'%';};
$('vol').onchange=e=>{const g=(e.target.value/100*4);C.volume=g;save('vol',g.toFixed(2));};
$('mute').onclick=()=>{C.mute=!C.mute;$('mute').classList.toggle('on',C.mute);save('mute',C.mute?1:0);};
[...$('chat').children].forEach(b=>b.onclick=()=>{C.chattiness=+b.dataset.v;fill();save('chat',b.dataset.v);});
$('qs').onchange=e=>save('qs',t2m(e.target.value));
$('qe').onchange=e=>save('qe',t2m(e.target.value));
$('wifi').onclick=()=>{if(confirm('Forget WiFi and restart into setup mode?'))fetch('/wifi_reset');};
let PLANTS=[];
async function loadPlants(){
  try{PLANTS=await(await fetch('/plants',{cache:'no-store'})).json();}catch(e){return;}
  const sel=$('plant');
  sel.innerHTML='<option value="">— choose your plant —</option>'+PLANTS.map(p=>'<option value="'+p.key+'">'+p.name+'</option>').join('');
  sel.value=C.plant||''; showCare();
  sel.onchange=()=>{C.plant=sel.value;save('plant',sel.value);showCare();};
}
function showCare(){
  const p=PLANTS.find(x=>x.key===$('plant').value), box=$('care');
  if(!p){box.style.display='none';return;}
  box.style.display='block';
  $('c_light').textContent=p.light; $('c_water').textContent=p.water;
  $('c_temp').textContent=p.tMin+'–'+p.tMax+'°C'; $('c_care').textContent=p.care;
}
(async()=>{await poll();fill();await loadPlants();setInterval(poll,3000);})();
</script></body></html>)HTML";
  server.send_P(200, "text/html", PAGE);
}

// Full plant list + care guides (the settings page builds the picker from this).
void handlePlants() {
  String j = "[";
  auto esc = [](const char *s) { String r; for (; *s; s++) { if (*s == '"' || *s == '\\') r += '\\'; r += *s; } return r; };
  for (int i = 0; i < PLANTS_N; i++) {
    const Plant &p = PLANTS[i];
    if (i) j += ",";
    j += "{\"key\":\"" + String(p.key) + "\",\"name\":\"" + esc(p.name) +
         "\",\"light\":\"" + esc(p.light) + "\",\"water\":\"" + esc(p.water) +
         "\",\"care\":\"" + esc(p.care) + "\",\"tMin\":" + String(p.tMin) +
         ",\"tMax\":" + String(p.tMax) + ",\"soilDry\":" + String(p.soilDry) +
         ",\"soilWet\":" + String(p.soilWet) + "}";
  }
  j += "]";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", j);
}

// Build the full status/settings + live-sensor JSON (used by /cfg AND the USB stream).
void statusJson(char *buf, size_t n) {
  auto toC = [](float v){ return USE_IMPERIAL ? (v - 32.0f) * 5.0f / 9.0f : v; };
  float outC = wx.valid ? toC(wx.temp) : 0, feelC = wx.valid ? toC(wx.feels) : 0;
  float hiC = wx.valid ? toC(wx.tMax) : 0, loC = wx.valid ? toC(wx.tMin) : 0;
  String sr = (wx.valid && wx.sunrise.length() >= 16) ? wx.sunrise.substring(11,16) : "";
  String ss = (wx.valid && wx.sunset.length()  >= 16) ? wx.sunset.substring(11,16)  : "";
  // JSON-escape the current spoken line (for the phone subtitle)
  char say[128]; { size_t j = 0; const char *s = vNowSaying.c_str();
    for (; *s && j < sizeof(say) - 2; s++) { if (*s == '"' || *s == '\\') say[j++] = '\\'; say[j++] = *s; } say[j] = 0; }
  unsigned long sayAge = vSayingAtMs ? (millis() - vSayingAtMs) / 1000 : 99999;
  snprintf(buf, n,
    "{\"volume\":%.2f,\"mute\":%s,\"quietStart\":%d,\"quietEnd\":%d,\"chattiness\":%d,\"plant\":\"%s\","
    "\"mood\":\"%s\",\"waterings\":%u,\"uptime_s\":%lu,\"wifi\":%s,\"rssi\":%d,\"internet\":%s,"
    "\"soil_pct\":%.0f,\"soil_raw\":%d,\"soil_status\":\"%s\","
    "\"dht_ok\":%s,\"temp_c\":%.1f,\"humidity\":%.0f,\"vpd\":%.2f,\"vpd_status\":\"%s\",\"dewpoint\":%.1f,"
    "\"light\":%s,"
    "\"wx_valid\":%s,\"out_temp_c\":%.1f,\"out_hum\":%.0f,\"uv\":%.1f,"
    "\"wx_code\":%d,\"wx_isday\":%d,\"wx_feels_c\":%.1f,\"wx_wind\":%.0f,\"wx_hi_c\":%.1f,\"wx_lo_c\":%.1f,"
    "\"wx_precip\":%.2f,\"wx_sr\":\"%s\",\"wx_ss\":\"%s\","
    "\"saying\":\"%s\",\"say_age\":%lu}",
    cfg.volume, cfg.mute ? "true" : "false", cfg.quietStart, cfg.quietEnd, cfg.chattiness, cfg.plant,
    emotion.c_str(), vWaterings, millis() / 1000UL,
    WiFi.status() == WL_CONNECTED ? "true" : "false", (int)WiFi.RSSI(), internetOK ? "true" : "false",
    isnan(sx.soilPct) ? 0 : sx.soilPct, sx.soilRaw, sx.soilStatus.c_str(),
    sx.dhtOK ? "true" : "false", isnan(sx.tempC) ? 0 : sx.tempC, isnan(sx.humidity) ? 0 : sx.humidity,
    isnan(sx.vpd) ? 0 : sx.vpd, sx.vpdStatus.c_str(), isnan(sx.dewC) ? 0 : sx.dewC,
    sx.lightOn ? "true" : "false",
    wx.valid ? "true" : "false", outC, wx.valid ? wx.humidity : 0, wx.valid ? wx.uvMax : 0,
    wx.valid ? wx.code : -1, wx.valid ? wx.isDay : 1, feelC, wx.valid ? wx.wind : 0, hiC, loC,
    wx.valid ? wx.precipSum : 0, sr.c_str(), ss.c_str(),
    say, sayAge);
}

// Apply one setting key=value (shared by the web API and the USB command line).
void applyCfgKV(const String &k, const String &v) {
  if      (k == "vol")   cfg.volume     = v.toFloat();
  else if (k == "mute")  cfg.mute       = (v == "1");
  else if (k == "qs")    cfg.quietStart = v.toInt();
  else if (k == "qe")    cfg.quietEnd   = v.toInt();
  else if (k == "chat")  cfg.chattiness = v.toInt();
  else if (k == "plant") strncpy(cfg.plant, v.c_str(), sizeof(cfg.plant) - 1);
  else if (k == "wifireset" && v == "1") wifiResetPending = true;  // -> reopen setup portal
}

// Handle a "CFG k=v k=v ..." command arriving over USB serial (from the bridge).
void handleSerialCmd(const String &line) {
  if (!line.startsWith("CFG ")) return;
  String rest = line.substring(4);
  bool changed = false;
  int start = 0;
  while (start < (int)rest.length()) {
    int sp = rest.indexOf(' ', start); if (sp < 0) sp = rest.length();
    String pair = rest.substring(start, sp);
    int eq = pair.indexOf('=');
    if (eq > 0) { applyCfgKV(pair.substring(0, eq), pair.substring(eq + 1)); changed = true; }
    start = sp + 1;
  }
  if (changed) { cfgSave(); cfgApplyVolume(); Serial.println("[CFG] applied via USB"); }
}

// Config API: no args = read; with args = update+save. Also returns live data.
void handleCfg() {
  bool changed = false;
  if (server.hasArg("vol"))  { cfg.volume = server.arg("vol").toFloat(); changed = true; }
  if (server.hasArg("mute")) { cfg.mute = server.arg("mute") == "1"; changed = true; }
  if (server.hasArg("qs"))   { cfg.quietStart = server.arg("qs").toInt(); changed = true; }
  if (server.hasArg("qe"))   { cfg.quietEnd = server.arg("qe").toInt(); changed = true; }
  if (server.hasArg("chat")) { cfg.chattiness = server.arg("chat").toInt(); changed = true; }
  if (server.hasArg("plant")){ strncpy(cfg.plant, server.arg("plant").c_str(), sizeof(cfg.plant)-1); changed = true; }
  if (changed) { cfgSave(); cfgApplyVolume(); }

  char buf[1200];
  statusJson(buf, sizeof(buf));
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", buf);
}

// ===========================================================================
// Cloud relay — let the owner see + control Terra from ANYWHERE, behind any
// WiFi / phone hotspot / NAT. Terra makes one outbound HTTPS POST that both
// uploads her status and returns any pending owner command in the response.
// (No inbound connection to the device is ever needed.)
// ===========================================================================
const char *CLOUD_URL = "https://terra-relay.wasifkarim.workers.dev/ingest?id=terra";
const char *CLOUD_KEY = "a140bae408bb79315ffbc34eb1192b70";

void cloudSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  char body[1200];
  statusJson(body, sizeof(body));
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https; https.setConnectTimeout(6000);
  if (!https.begin(client, CLOUD_URL)) return;
  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Terra-Key", CLOUD_KEY);
  int code = https.POST((uint8_t *)body, strlen(body));
  if (code == 200) {
    String resp = https.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, resp)) {
      const char *cmd = doc["cmd"] | "";
      if (cmd && cmd[0]) {                       // owner changed a setting from the web
        Serial.printf("[CLOUD] cmd: %s\n", cmd);
        handleSerialCmd(String("CFG ") + cmd);   // reuse the existing command parser
      }
    }
  } else {
    Serial.printf("[CLOUD] POST failed: %d\n", code);
  }
  https.end();
}

void setup() {
  Serial.begin(115200);
  const unsigned long start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  Serial.println("\n===========================================");
  Serial.println(" PetPlant — weather/time/location dashboard");
  Serial.println("===========================================");

  pinMode(LED_BUILTIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  dht.setup(DHT_PIN, DHTesp::DHT11);
  pinMode(LIGHT_PIN, INPUT);
  Serial.printf("[SENSOR] soil A0/D0, DHT11 D2, light D1(GPIO%d)\n", LIGHT_PIN);

  if (LittleFS.begin(true)) Serial.println("[FS] LittleFS mounted (voice clips)");
  else                      Serial.println("[FS] LittleFS mount FAILED");

  // Power-cycle 3x to reset WiFi — the cable-free, no-app fallback. Each boot
  // bumps a counter; a boot that runs >8s clears it (see loop). 3 quick cycles
  // (each under 8s) forget the saved WiFi so she opens the "Terra-Setup" portal.
  {
    Preferences bp; bp.begin("boot", false);
    int bc = bp.getInt("cnt", 0) + 1;
    bp.putInt("cnt", bc); bp.end();
    if (bc >= 3) {
      Preferences b2; b2.begin("boot", false); b2.putInt("cnt", 0); b2.end();
      Serial.println("[WiFi] 3x power-cycle -> forgetting WiFi, opening setup portal");
      WiFiManager wm; wm.resetSettings();   // connectWiFi() below will now open the portal
    } else {
      Serial.printf("[BOOT] power-on count %d (3 in a row resets WiFi)\n", bc);
    }
  }

  randomSeed(esp_random());
  voiceBegin();          // on-device voice engine (I2S audio + phrasebank + NVS memory)

#if ENABLE_DISPLAY
  displaySetup();       // panel driver chosen by DISPLAY_DRIVER (v1 = GC9A01 round)
#endif

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    geolocate();
    syncTime();
    fetchWeather();

    server.on("/", handleSettings);          // customer control panel (aiko.local)
    server.on("/settings", handleSettings);
    server.on("/cfg", handleCfg);            // read/update settings + live data
    server.on("/plants", handlePlants);      // plant list + care guides
    server.on("/wifi_reset", []() { server.send(200, "text/plain", "resetting…"); delay(400); resetWiFi(); });
    server.on("/dashboard", handleRoot);     // developer dashboard (face/voice preview)
    server.on("/api", handleApi);
    server.on("/face", handleFace);
    server.on("/say", []() {                         // dashboard pushes current spoken line for the display caption
      if (server.hasArg("t"))
        displaySetCaption(server.arg("t"), server.hasArg("ms") ? server.arg("ms").toInt() : 5000);
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "text/plain", "ok");
    });
    // Serve voice clips from LittleFS with the correct MIME type (audio/mpeg).
    server.onNotFound([]() {
      String u = server.uri();
      if (u.startsWith("/audio/") && u.endsWith(".mp3") && LittleFS.exists(u)) {
        File f = LittleFS.open(u, "r");
        server.sendHeader("Cache-Control", "max-age=86400");
        server.streamFile(f, "audio/mpeg");
        f.close();
        return;
      }
      server.send(404, "text/plain", "not found");
    });
    server.begin();
    Serial.print("[HTTP] Dashboard ready at  http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
  }
}

void loop() {
  voiceLoop();            // pump audio + run the standalone voice scheduler (call often!)
  server.handleClient();

  // Owner asked (from the dashboard) to re-provision WiFi -> forget + reboot into
  // the "Terra-Setup" portal so a new network can be chosen.
  if (wifiResetPending) {
    Serial.println("[WiFi] remote reset requested -> opening setup portal");
    Serial.flush(); delay(300);
    resetWiFi();          // clears creds + restarts (never returns)
  }

  // A boot that stays up >8s counts as "good" — clear the power-cycle counter so
  // only 3 *quick* cycles in a row trigger the WiFi reset.
  static bool bootCntCleared = false;
  if (!bootCntCleared && millis() > 8000) {
    bootCntCleared = true;
    Preferences bp; bp.begin("boot", false); bp.putInt("cnt", 0); bp.end();
  }

  // Accept "CFG k=v ..." commands over USB (lets the settings page work via the cable).
  static String serialBuf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n')      { handleSerialCmd(serialBuf); serialBuf = ""; }
    else if (c != '\r') { if (serialBuf.length() < 200) serialBuf += c; }
  }

#if ENABLE_DISPLAY
  // Render the face to the GC9A01 at ~25 fps.
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame >= 40) {
    lastFrame = millis();
    bool offline = (WiFi.status() != WL_CONNECTED) || !internetOK;
    displayRenderFace(emotion, offline, millis());
  }
#endif

  // Heartbeat + sensor read every 1s (non-blocking).
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 1000) {
    lastBeat = millis();
    readSensors();
    decideEmotion();
    counter++;
    digitalWrite(LED_BUILTIN, counter & 1);   // non-blocking heartbeat (no delay -> clean audio)
    Serial.printf("soil=%d (%.0f%% %s)  temp=%.1fC hum=%.0f%% VPD=%.2f(%s) dht=%s light=%s\n",
                  sx.soilRaw, isnan(sx.soilPct)?0:sx.soilPct, sx.soilStatus.c_str(),
                  isnan(sx.tempC)?0:sx.tempC, isnan(sx.humidity)?0:sx.humidity,
                  isnan(sx.vpd)?0:sx.vpd, sx.vpdStatus.c_str(), sx.dhtOK?"ok":"FAIL",
                  sx.lightOn?"bright":"dark");
    // Full status over USB so a laptop can show the live web page without WiFi.
    char dj[1200]; statusJson(dj, sizeof(dj));
    Serial.printf("[DATA]%s\n", dj);
  }

  // Non-blocking WiFi reconnect if the link drops (uses the saved credentials).
  static unsigned long lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry >= 30000) {
    lastWifiTry = millis();
    WiFi.reconnect();        // async, doesn't block audio
  }

  // Cloud relay push (every ~7s, but never while she's speaking so audio stays
  // smooth). One POST uploads status + pulls any owner command back.
  static unsigned long lastCloud = 0;
  if (WiFi.status() == WL_CONNECTED && !vBusy() && millis() - lastCloud >= 7000) {
    lastCloud = millis();
    cloudSync();
  }

  // Background data refresh (only when WiFi up). Retry failed steps every minute,
  // and refresh weather every 10 minutes.
  static unsigned long lastTry = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastTry >= RETRY_INTERVAL_MS) {
    bool due = !wx.valid || (millis() - wx.lastUpdate >= WEATHER_INTERVAL_MS);
    if (!geo.valid)            { lastTry = millis(); geolocate(); }
    else if (!timeSynced)      { lastTry = millis(); syncTime(); }
    else if (due)              { lastTry = millis(); fetchWeather(); }
  }
}
