---
name: m5paper
description: Develop firmware for M5Paper v1.1 (ESP32 e-ink device) using PlatformIO and the M5EPD library. Covers display API, colors, rotation, WiFi, sensors, power management, buttons, and flashing. Use when building M5Paper projects, working with e-ink displays, or troubleshooting M5Paper firmware.
---

# M5Paper Development

## Hardware summary
- **MCU**: ESP32-D0WDQ6-V3, 240MHz dual-core, 520KB SRAM, 8MB PSRAM, 16MB Flash
- **Display**: 4.7" e-ink, 960x540 native landscape, 16 grayscale, IT8951 controller
- **Touch**: GT911 capacitive, 2-point
- **Sensors**: SHT30 (temp/humidity), BM8563 (RTC)
- **Battery**: 1150mAh LiPo, ~25µA deep sleep
- **USB**: CP2104 USB-to-serial (Type-C)
- **WiFi**: 2.4 GHz only (ESP32 limitation)
- **Buttons**: rear reset button + side 3-way toggle (click/left/right)

## PlatformIO setup

```ini
[env:m5paper]
platform = espressif32
board = m5stack-fire
framework = arduino
upload_speed = 2000000
monitor_speed = 115200
board_build.partitions = default_16MB.csv
build_flags =
  -std=gnu++17
  -Ofast
  -DBOARD_HAS_PSRAM
  -mfix-esp32-psram-cache-issue
build_unflags =
  -std=gnu++11
lib_deps =
  m5stack/M5EPD
  bblanchon/ArduinoJson
```

- Use `board = m5stack-fire` (no official m5paper board definition)
- PlatformIO needs pip in venv. If using uv: `uv pip install pip`

### This repo (`m5paper-dash-trading212`)

PlatformIO Core is installed in the **repository root** Python venv, not inside `firmware/`. The `firmware/` directory only holds the PIO project (`platformio.ini`, `src/`).

```bash
# from repo root
uv venv .venv
source .venv/bin/activate
uv pip install pip platformio

cd firmware
pio run              # build
pio run -t upload    # flash
pio device monitor   # serial @ 115200
```

If `pio` is not on your PATH, call it explicitly: **`.venv/bin/pio`** from repo root (still run `cd firmware` first so PIO finds `platformio.ini`).

## Display — critical gotchas

### Rotation (use 0-3 only, NOT degrees)
| Value | Orientation |
|-------|-------------|
| **`SetRotation(0)`** | **Landscape, USB/rocker on top (use this for standing)** |
| `SetRotation(2)` | Landscape, right-side up |
| `SetRotation(1)` | Portrait |
| `SetRotation(3)` | Portrait inverted |

**Never pass degree values** like 90 — they get interpreted as `90 % 4`.

### Colors (INVERTED from most docs/examples)
- **0 = white, 15 = black** (opposite of what most docs say)
- Confirmed by testing: `fillCanvas(0)` = white, `fillCanvas(15)` = black

```cpp
#define C_WHITE 0
#define C_BLACK 15
#define C_DARK  12
#define C_MID   8
#define C_LIGHT 3
```

### Canvas workflow
```cpp
M5EPD_Canvas canvas(&M5.EPD);
canvas.createCanvas(960, 540);
canvas.fillCanvas(C_WHITE);
canvas.setTextSize(5);
canvas.setTextColor(C_BLACK);
canvas.setTextDatum(TC_DATUM);
canvas.drawString("Hello", 480, 100);
canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
```

### Update modes
| Mode | Speed | Quality | Use for |
|------|-------|---------|---------|
| `UPDATE_MODE_GC16` | ~1s | Best grayscale | Full refresh |
| `UPDATE_MODE_DU4` | ~0.3s | 4-level gray | Partial updates |
| `UPDATE_MODE_DU` | Fastest | B&W only | Text-only UI |

### Text sizing
- `setTextSize(n)` scales the built-in bitmap font (1–7)
- Size 7 is large and readable on e-ink at 235 PPI
- For nicer fonts: `loadFont()` + `createRender(size)` for TTF support

## Sensors

```cpp
// Temperature & humidity
M5.SHT30.UpdateData();
float temp = M5.SHT30.GetTemperature();
float hum  = M5.SHT30.GetRelHumidity();  // NOT GetHumidity()

// Battery
uint32_t mv = M5.getBatteryVoltage();  // 3300=empty, 4200=full

// RTC
rtc_time_t time; rtc_date_t date;
M5.RTC.getTime(&time); M5.RTC.getDate(&date);
```

## WiFi + HTTP

```cpp
// Optional: clear stale STA state after RTC wake (do NOT use eraseap=true — wipes NVS WiFi)
WiFi.disconnect(true, false);
WiFi.mode(WIFI_OFF);
delay(100);

WiFi.mode(WIFI_STA);
WiFi.setSleep(false);
WiFi.begin("SSID", "PASS");

unsigned long t0 = millis();
while (WiFi.status() != WL_CONNECTED && millis() - t0 < 45000UL) {
    esp_task_wdt_reset();  // feed WDT — marginal WiFi can exceed default task WDT window
    delay(250);
}

HTTPClient http;
http.begin("http://host:port/path");
if (http.GET() == 200) {
    String payload = http.getString();
    // parse with ArduinoJson
}
http.end();
WiFi.disconnect(true);
```

### Stuck on "BOOTING..." (screen never changes)

The BOOTING splash is replaced only **after** WiFi connects (or times out → "WiFi Failed"). So a frozen BOOTING image almost always means **the device is still in the WiFi association phase** (or crashed/rebooted before the next full refresh).

1. **USB serial** (`pio device monitor`, 115200): confirm whether you see `WiFi: connected` vs repeated `WiFi status=...` vs reboot loop (Guru Meditation).
2. **Task WDT**: If WiFi or the stack blocks too long without `esp_task_wdt_reset()`, the ESP32 resets — you may mostly *see* BOOTING between panics. Feed the WDT in the WiFi wait loop on long timeouts.
3. **Longer WiFi deadline**: Weak 2.4 GHz (distance, AP load) may need **>10s**; use a **millis() deadline** (e.g. 45s), not only a small fixed attempt count.
4. **Do not** pass `eraseap=true` to `WiFi.disconnect()` — that erases stored WiFi credentials in NVS.
5. **Not NTP**: NTP/`syncTime()` runs after the dashboard draw in this project; a hang there would show **Fetching...** or the dashboard, not BOOTING (see below).

### Battery: NTP must run AFTER draw

**Critical for battery operation**: If you use NTP (`syncTime()`, `configTzTime()`, `getLocalTime()`), call it **after** drawing the display, not before. On battery, NTP to the internet can block indefinitely. If syncTime runs before parse/draw, the firmware never reaches the display update — screen stays on "fetching" or boot screen.

**Correct order**: fetch → parse → **draw** → syncTime → disconnect → sleep

**Wrong order** (causes apparent "fetch hang" on battery): fetch → syncTime → parse → draw

## Power management

### Use `M5.shutdown(seconds)` — NOT ESP32 deep sleep

```cpp
void goToSleep() {
    M5.shutdown(REFRESH_MINS * 60);  // sets RTC alarm, powers off
    // Won't return on battery (device is OFF).
    // On USB, returns because USB keeps ESP32 alive — handle in loop().
}

void setup() {
    M5.begin();
    M5.EPD.SetRotation(0);
    // ... WiFi, fetch, draw, then goToSleep()
}

void loop() {
    // Fallback for USB: M5.shutdown() can't fully power off on USB.
    static unsigned long start = millis();
    if (millis() - start > (unsigned long)REFRESH_MINS * 60 * 1000)
        ESP.restart();
    M5.shutdown(REFRESH_MINS * 60);
    delay(30000);
}
```

**On battery**: `M5.shutdown()` cuts GPIO2 (power rail off, device fully OFF, ~0µA). RTC alarm fires → cold boot from `setup()`. E-ink retains image while off.

**On USB**: `shutdown()` can't fully power off. `loop()` re-arms the alarm and calls `ESP.restart()` after the interval.

### Why NOT ESP32 deep sleep

We tested `esp_deep_sleep_start()` with `gpio_hold_en` on GPIO2. **Unreliable:**
- `gpio_hold_dis()` at boot releases GPIO2 → power rail drops → display dies → device appears bricked
- Timer wake was intermittent
- Button wake via `ext0_wakeup(GPIO39)` never worked
- Adding ext0 config sometimes broke timer wake too

**Do not use `esp_deep_sleep_start()` on M5Paper.** Use `M5.shutdown()` instead.

## Buttons

```cpp
M5.update();
if (M5.BtnL.wasPressed()) { /* side toggle left  (GPIO39) */ }
if (M5.BtnP.wasPressed()) { /* side toggle click  (GPIO38) */ }
if (M5.BtnR.wasPressed()) { /* side toggle right (GPIO37) */ }
```

- **Rear button (reset)**: **USB plugged in** → classic ESP32 reset (reboots firmware). **Battery + already shut down** (`M5.shutdown()` cut power) → **does nothing useful** — ESP32 has no VCC, so there is no reset to trigger; use **RTC alarm**, **plug USB**, or **long-press PWR** (rotary center ~2s) to turn on ([M5EPD #9](https://github.com/m5stack/M5EPD/issues/9)). While still awake on battery, rear reset may still reboot the chip.
- **Side toggle** (GPIO37/38/39): only works while ESP32 is powered. **Long-press center (PWR ~2s)** can power the unit on from full shutdown; left/right/short click need the CPU running your `loop()`.

## USB serial
- macOS: `/dev/cu.usbserial-*` (port name changes on reconnect)
- Linux: `/dev/ttyUSB*`
- Windows: `COM*`

## Common mistakes
1. Using degree values for rotation (use 0-3)
2. Assuming 0=black, 15=white (it's inverted: 0=white, 15=black)
3. Calling `GetHumidity()` instead of `GetRelHumidity()`
4. Using `board = m5paper` in PlatformIO (use `m5stack-fire`)
5. Forgetting `fillCanvas()` before redraw (canvas retains content)
6. Not adding timeout to WiFi connection loop
7. Using `esp_deep_sleep_start()` instead of `M5.shutdown()`
8. Not handling USB case in `loop()` when using `M5.shutdown()`
9. Running syncTime/NTP before draw on battery
