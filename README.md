# Floating Skull LEDs

ESP32-S3 + FastLED project driving **two concentric LED rings** (122 total WS2812B) with a built-in web UI, painter, effect auto-cycle, and OTA updates.  
All animations are authored once on a **61-LED primary geometry**; the **second 61-LED ring mirrors the primary** every frame (facing mirror: 12 o’clock aligned, direction reversed).

---

## Features

- **Primary + Mirror**: 61-LED primary (center-out rings), plus a second 61-LED ring that auto-mirrors the primary each frame.
- **True ring painter (61px)**: Web page shows the 5 concentric rings; paint by tapping/dragging.
- **Built-in effects** with auto-cycle timer (default: every 3 minutes).
- **Brightness control** from the UI.
- **Button control** (short = full reset, long = power toggle).
- **mDNS + OTA**: Update over Wi-Fi at `http://floating-skull.local/`.

---

## Hardware

- **MCU**: ESP32-S3 (e.g., Seeed XIAO ESP32S3)
- **LEDs**: WS2812B (GRB)
  - **Total**: 122
  - **Primary geometry (indices 0..60):**
    - R4 (outer): 24  
    - R3: 16  
    - R2: 12  
    - R1: 8  
    - R0 (center): 1
  - **Mirror geometry (indices 61..121)**: automatic facing mirror of the primary
- **Power**: 5 V supply sized for your brightness/usage  
  The code caps FastLED current via:
  ```cpp
  #define VOLTS 5
  #define MAX_MA 10000
  ```
  Adjust `MAX_MA` for your PSU and wiring.
- **Pins**
  - LED data: **GPIO21**
  - Button (momentary to GND): **GPIO9** with `INPUT_PULLUP`

**Wiring tips**
- Put a **330 Ω** series resistor on the LED data line and a **large electrolytic** (e.g., 1000 µF, 6.3 V+) across 5 V and GND near the LEDs.
- Tie all grounds together (PSU GND ↔ ESP32 GND ↔ LED GND).
- Long runs benefit from power injection.

---

## Controls

### Physical Button (GPIO9 → GND)
- **Short press** (< ~1000 ms): **Full reset** (`ESP.restart()`).
- **Long hold** (≥ ~1000 ms): **Toggle power** (OFF/ON latch).  
  When powering on, all pixels flash dark blue briefly.

### Web UI
- Open `http://floating-skull.local/` (or the ESP32’s IP).
- Buttons for **effect select**, **auto-cycle**, **all off**, and **paint**.
- **Brightness** slider (5–255).

### Painter (61 LEDs)
- Shows only the **primary** 61-LED concentric layout (center + 4 rings).
- Painting a primary pixel automatically mirrors to the second ring.

---

## Web Endpoints

- `/` — Main controller UI
- `/paint` — Concentric ring painter (61px)
- `/set?anim=X` — Select effect `X`  
  - `255` = Auto cycle  
  - `254` = All off (hard off)
- `/setb?b=NNN` — Set brightness (5..255)
- `/api/status` — JSON status: power, effect index/name, paint mode, brightness
- `/setpixel?idx=I&hex=RRGGBB` — Paint one pixel  
  - `idx` expects **primary** indices (0..60) from the painter; the firmware mirrors it to 61..121 automatically.
- `/diag/step` — Steps a single white pixel around the **primary** for diagnosis

---

## Effects (rendered on the primary, mirrored automatically)

1. **Center Pulse** — breathing glow from center outward  
2. **Ripple Rings** — expanding/contracting ring bands  
3. **Ring Chase** — chasing heads per ring with falloff  
4. **Polar Swirl** — hue swirl by angle around each ring  
5. **Core Waves** — wave interference across rings  
6. **Outer Comet** — comet on the outer ring with ambient base

Global tempo divider: `SPEED_DIV` (larger = slower; default `3`).

Auto-cycle dwell time: `EFFECT_DWELL_MS` (default **3 minutes**).

---

## Build & Flash

1. **Arduino IDE / PlatformIO** with ESP32 support.
2. Install **FastLED**.
3. Create a `secrets.h` in the sketch folder:
   ```cpp
   #pragma once
   #define WIFI_SSID "YourSSID"
   #define WIFI_PASS "YourPass"
   ```
4. Select your ESP32-S3 board, set the correct **USB/serial port**, and flash.

> OTA is enabled as `floating-skull`. After first flash via USB, you can upload via network from the IDE’s **Network Ports** list.

---

## How mirroring works

- All animation and painting target the **primary** range `[0..60]` using the defined ring geometry.
- For every primary pixel `(ring r, position k)`, the mirror pixel is computed at the **same ring** with **reversed position** (facing mirror), producing a symmetrical look with both rings aligned at **12 o’clock**.
- The painter UI emits indices only for the primary; the firmware mirrors to `61..121` automatically.

---

## Configuration Summary

```cpp
// LED config
#define LED_PIN     21
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define NUM_LEDS    122
#define NUM_PRIMARY 61

// Power
#define VOLTS   5
#define MAX_MA  10000
#define MASTER_BRIGHTNESS 255

// Button
#define BTN_PIN 9
#define HOLD_MS 1000

// Wi-Fi / mDNS
AP_SSID     = "FloatingSkull"
AP_PASS     = "skullskull"
MDNS_NAME   = "floating-skull"  // http://floating-skull.local/
```

---

## Troubleshooting

- **Nothing lights**: check 5 V polarity, common ground, data resistor, correct **GPIO21**.
- **Glitching**: lower brightness, confirm `MAX_MA` vs PSU capacity, shorten data line or add a buffer, ensure a 1000 µF cap at the LEDs.
- **Web not reachable**: connect to the ESP’s AP (`FloatingSkull / skullskull`) and browse `http://192.168.4.1/`. Check your router for the mDNS hostname.
- **Painter offset**: verify your physical LED ring **starting index** aligns at 12 o’clock; the mirror math assumes that reference.

---

## License

MIT (or your preference).
