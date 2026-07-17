// ===========================================================================
// Sensor read test — Soil moisture (analog) + DHT11 (temperature + humidity)
// Board: Seeed Studio XIAO ESP32S3
//
// Wiring:
//   Soil AOUT  -> D0  (GPIO1)   |  Soil VCC  -> 3V3,  Soil GND  -> GND
//   DHT11 DATA -> D2  (GPIO3)   |  DHT11 VCC -> 3V3,  DHT11 GND -> GND
//   Light DO   -> D1  (GPIO2)   |  Light VCC -> 3V3,  Light GND -> GND
//
// Open the serial monitor at 115200 baud to watch the readings update.
// ===========================================================================
#include <Arduino.h>
#include <DHTesp.h>
#include <driver/i2s.h>          // legacy I2S (Arduino core 2.x) for the MAX98357A amp
#include <Wire.h>
#include <Adafruit_SSD1306.h>    // I2C OLED display

const int SOIL_PIN  = 1;   // D0  (analog AOUT)
const int DHT_PIN   = 3;   // D2  (DHT11 data)
const int LIGHT_PIN = 2;   // D1  (light module DO — digital: LOW=light, HIGH=dark)

// I2C OLED display:  SDA -> D9 (GPIO8),  SCL -> D8 (GPIO7)
#define OLED_SDA  8
#define OLED_SCL  7
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOK = false;

uint8_t i2cScan() {                // returns the first I2C address found (0 = none)
  uint8_t first = 0;
  Serial.print("[I2C] scanning D9/D8... ");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("found 0x%02X ", a); if (!first) first = a; }
  }
  Serial.println(first ? "" : "NONE (check SDA/SCL/VCC/GND)");
  return first;
}

// Audio amp (MAX98357A):  BCLK=D4(GPIO5)  LRC=D3(GPIO4)  DIN=D5(GPIO6)
#define I2S_BCLK 5
#define I2S_LRC  4
#define I2S_DOUT 6
#define I2S_RATE 16000

void audioSetup() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = I2S_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = { .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// play a sine tone of `freq` Hz for `ms` milliseconds
void tone2(float freq, int ms, float vol = 0.4) {
  static double phase = 0; double step = 2.0 * PI * freq / I2S_RATE;
  int total = I2S_RATE * ms / 1000; int16_t buf[128]; size_t bw; int done = 0;
  while (done < total) {
    int n = total - done; if (n > 128) n = 128;
    for (int i = 0; i < n; i++) { buf[i] = (int16_t)(sin(phase) * 32767 * vol);
      phase += step; if (phase > 2 * PI) phase -= 2 * PI; }
    i2s_write(I2S_NUM_0, buf, n * sizeof(int16_t), &bw, portMAX_DELAY);
    done += n;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// a little tune — "Ode to Joy" (first phrase)
void playMelody() {
  struct N { int f, ms; };
  static const N song[] = {
    {330,280},{330,280},{349,280},{392,280},   {392,280},{349,280},{330,280},{294,280},
    {262,280},{262,280},{294,280},{330,280},   {330,420},{294,140},{294,560},
  };
  for (auto &n : song) { tone2(n.f, n.ms, 1.0); delay(25); }   // 100% digital volume
}

// Soil calibration (raw ADC values). To calibrate YOUR probe, watch the raw
// number: hold it in open air -> that's SOIL_AIR_RAW (0%); dunk it in water to
// the line -> that's SOIL_WATER_RAW (100%). Then update these two numbers.
const int SOIL_AIR_RAW   = 2550;   // ~0%   (dry / in air)
const int SOIL_WATER_RAW = 1190;   // ~100% (in water)

DHTesp dht;

int soilPercent(int raw) {
  long span = SOIL_AIR_RAW - SOIL_WATER_RAW;
  if (span == 0) return -1;
  long pct = 100L * (SOIL_AIR_RAW - raw) / span;
  return (int)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // wait briefly for USB serial

  analogReadResolution(12);                      // 0..4095
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);   // read the full ~0..3.3 V range
  dht.setup(DHT_PIN, DHTesp::DHT11);
  pinMode(LIGHT_PIN, INPUT);                     // light module DO
  audioSetup();                                  // I2S amp

  // --- I2C OLED display ---
  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t addr = i2cScan();
  if (!addr) addr = 0x3C;                         // SSD1306 default
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, addr);
  if (oledOK) {
    oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2); oled.setCursor(24, 10); oled.print("TERRA");
    oled.setTextSize(1); oled.setCursor(16, 42); oled.print("display OK :)");
    oled.display();
    Serial.printf("[OLED] SSD1306 ready @ 0x%02X\n", addr);
  } else {
    Serial.println("[OLED] init FAILED -- if the scan found an address, it may be an SH1106 (tell me)");
  }

  Serial.println("\n=== Soil + DHT11 + Light + Audio + Display test ===");
  Serial.println("Soil -> D0   DHT11 -> D2   Light -> D1   Audio -> D3/D4/D5");
  Serial.println("Playing a little tune (Ode to Joy)... \xF0\x9F\x8E\xB5\n");
  playMelody();
}

void loop() {
  // --- Soil moisture (analog) ---
  int raw = analogRead(SOIL_PIN);
  int pct = soilPercent(raw);

  // --- DHT11 (temperature + humidity) ---
  TempAndHumidity th = dht.getTempAndHumidity();
  bool dhtOK = (dht.getStatus() == DHTesp::ERROR_NONE) && !isnan(th.temperature);

  // --- Light module (digital: LOW = light, HIGH = dark) ---
  bool lightOn = (digitalRead(LIGHT_PIN) == LOW);

  Serial.printf("Soil: raw=%4d (%3d%%)   |   ", raw, pct);
  if (dhtOK)
    Serial.printf("DHT11: %.1f C / %.1f F, %.0f%% RH   |   ",
                  th.temperature, th.temperature * 9.0 / 5.0 + 32.0, th.humidity);
  else
    Serial.printf("DHT11: FAILED (%s)   |   ", dht.getStatusString());
  Serial.printf("Light: %s (DO=%s)\n", lightOn ? "bright" : "dark", lightOn ? "LOW" : "HIGH");

  // --- live sensor readout on the OLED ---
  if (oledOK) {
    oled.clearDisplay();
    oled.setTextSize(2); oled.setCursor(28, 0); oled.print("TERRA");
    oled.drawLine(0, 18, 127, 18, SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 24); oled.printf("Soil : %d %%", pct);
    oled.setCursor(0, 36);
    if (dhtOK) oled.printf("Temp : %.0f C  %.0f%%", th.temperature, th.humidity);
    else       oled.print("Temp : -- (no DHT)");
    oled.setCursor(0, 48); oled.printf("Light: %s", lightOn ? "bright" : "dark");
    oled.display();
  }

  // replay the tune every ~5th reading so you can hear it clearly
  static int cyc = 0;
  if (++cyc % 5 == 0) { Serial.println("   🎵 playing tune..."); playMelody(); }

  delay(2000);   // the DHT11 needs ~2 s between reads
}
