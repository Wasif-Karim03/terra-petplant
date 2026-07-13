# PetPlant (Aiko) — Final Wiring / Pinout Reference

Board: **Seeed Studio XIAO ESP32S3**
Golden rule: **power every part from `3V3`, not `5V`** (the amp is the only optional 5V exception). All parts connect **directly — no level shifters/converters**.

> The XIAO silkscreen labels pins **D0–D10** (there are no "A" labels). GPIO numbers are given for reference (sometimes printed on the back).

---

## 1) By component (what wires where)

### 🖥️ Display — GC9A01 (1.28" round, SPI)
| Display pin | XIAO pad | GPIO |
|---|---|---|
| VCC | 3V3 | — |
| GND | GND | — |
| SCL (clock) | **D8** | GPIO7 |
| SDA (MOSI) | **D10** | GPIO9 |
| DC | **D6** | GPIO43 |
| CS | **D7** | GPIO44 |
| RST | **D9** | GPIO8 |

*(No backlight pin — always on. "SDA/SCL" here are SPI names, not I²C.)*

### 🔊 Audio — MAX98357A (I²S amp) + speaker
| Amp pin | XIAO pad | GPIO |
|---|---|---|
| VIN | **5V** (or 3V3) | — |
| GND | GND | — |
| LRC (LRCLK) | **D3** | GPIO4 |
| BCLK | **D4** | GPIO5 |
| DIN | **D5** | GPIO6 |
| Speaker +/− | → amp's speaker terminals | — |

*(GAIN and SD pins: leave at default / unconnected. 5V gives more volume; 3V3 also works.)*

### 🌱 Soil moisture sensor (capacitive, analog)
| Sensor pin | XIAO pad | GPIO |
|---|---|---|
| VCC | **3V3** ⚠️ (never 5V — protects the ADC) | — |
| GND | GND | — |
| AOUT | **D0** | GPIO1 |

### 🌡️ DHT11 — temperature + humidity (digital)
| DHT11 pin | XIAO pad | GPIO |
|---|---|---|
| + / VCC | 3V3 | — |
| out / DATA | **D2** | GPIO3 |
| − / GND | GND | — |

### 💡 Light sensor (digital, 3-pin DO module) — ✅ tested working
| Sensor pin | XIAO pad | GPIO |
|---|---|---|
| VCC | 3V3 | — |
| GND | GND | — |
| DO | **D1** | GPIO2 |

Reads: **DO LOW = light**, **DO HIGH = dark** (threshold set by the onboard trimpot). Binary only (no gradient).

---

## 2) By XIAO pin (every pin, at a glance)

| XIAO pad | GPIO | Connected to | Type |
|---|---|---|---|
| **D0** | 1 | Soil AOUT | analog in |
| **D1** | 2 | Light DO | digital in |
| **D2** | 3 | DHT11 DATA | digital 1-wire |
| **D3** | 4 | Audio LRC | I²S |
| **D4** | 5 | Audio BCLK | I²S |
| **D5** | 6 | Audio DIN | I²S |
| **D6** | 43 | Display DC | SPI |
| **D7** | 44 | Display CS | SPI |
| **D8** | 7 | Display SCL | SPI clock |
| **D9** | 8 | Display RST | SPI |
| **D10** | 9 | Display SDA | SPI MOSI |
| **5V** | — | Audio VIN (optional) | power |
| **3V3** | — | Display, Soil, DHT11, Light (VCC) | power |
| **GND** | — | ALL grounds (common) | power |

✅ **All 11 GPIO pins used, zero conflicts.**

---

## 3) Power distribution (important for soldering)

Multiple parts share **3V3** and **GND**, but the XIAO has only one 3V3 pad and a couple of GND pads. So on the solder board:

- Make a **common 3V3 rail** → Display, Soil, DHT11, Light all tap it.
- Make a **common GND rail** → everything ties to it (including the amp and speaker return).
- **Audio VIN** → the **5V** pad (separate from the 3V3 rail) for best volume.

Current is comfortably within the XIAO's regulator (display ~80 mA is the biggest draw; sensors are tiny; the amp pulls from 5V on audio peaks).

---

## 4) Status
- 💡 Light sensor — **tested ✅** (2025-07-10) · DO LOW=light, HIGH=dark
- 🌱 Soil moisture — **tested ✅** (2025-07-10) · dry(air) ~2550, wet(water) ~1190 raw
- 🌡️ DHT11 — **tested ✅** (2025-07-10) · ~26 °C / ~66% RH, responds to breath
- 🖥️ Display (GC9A01) — **tested ✅** (2025-07-10) · colors correct with invert=true, rgb_order=false
- 🔊 Audio (MAX98357A + speaker) — **tested ✅** (2025-07-10) · beeps play (powered VIN from 3V3; legacy driver/i2s.h, mono, GPIO4=WS/D3, GPIO5=BCLK/D4, GPIO6=DOUT/D5)

**ALL COMPONENTS BENCH-TESTED ✅ — ready for integration + soldering.**

*Test sketches live in `src/tests/` (built via the `test_*` PlatformIO envs). Delete `src/tests/` + the `test_*` envs before the final build.*
