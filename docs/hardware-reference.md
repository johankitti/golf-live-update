# Hardware reference — ESP32-S3-Zero + 64×64 HUB75 panel

Portable hardware setup for this rig. The **software** on top can be anything;
this document covers only the board, panel, wiring, and power — everything you
need to reuse the hardware for a different display project.

---

## 1. Bill of materials

| Part | Detail | Link |
|---|---|---|
| MCU board | **Waveshare ESP32-S3-Zero** (ESP32-S3FH4R2, 4 MB flash / 2 MB PSRAM, USB-C) | https://www.waveshare.com/wiki/ESP32-S3-Zero · https://www.espboards.dev/esp32/esp32-s3-zero/ |
| Display | **64×64 P2 HUB75 RGB LED matrix** (128×128 mm), Electrokit | https://www.electrokit.com/en/full-color-panel-2mm-rgb-led-matrix-64x64px-128x128mm-p2 |
| Power supply | **5 V DC, ≥2 A (3 A ideal), center-positive, 5.5×2.1 mm barrel** | e.g. https://www.electrokit.com/en/batterieliminator-5v-3a |
| Header pins | 2× straight pin-header strips (come with the board) | — |
| Jumper wires | A few female-Dupont + 2 for power | — |

Board images (in this repo): `docs/esp32-s3-zero-board.png` (photo),
`docs/esp32-s3-zero-pinout.png` (labeled pinout).
Pinout source: `https://cdn.espboards.net/boards/esp32-s3-zero/pinout-1098.png`

### What the panel ships with (Electrokit box)
- 1 m power cable → 2× **VH4** connectors (into the panel)
- 200 mm 16-pin **IDC** cable
- 300 mm cable: 16-pin IDC → **Dupont female** leads (this is what connects to the MCU)
- **DC-terminal adapter** (barrel-jack socket + `+`/`−` screw terminals)
- Mounting screws

---

## 2. The board — pin layout

Waveshare ESP32-S3-Zero, **USB-C at top**, front (chip) side. The two solderable
side rows are 9 pins each (18 total):

```
        [ USB-C ]
 LEFT (top→bottom)      RIGHT (top→bottom)
   5V                     TX  (GPIO43)
   GND                    RX  (GPIO44)
   3V3                    GPIO13
   GPIO1                  GPIO12
   GPIO2                  GPIO11
   GPIO3                  GPIO10
   GPIO4                  GPIO9
   GPIO5                  GPIO8
   GPIO6                  GPIO7
```
- GPIO14/15/16 and 17/18/38–45 live on the **bottom edge / back** — harder to
  reach; the pin map below avoids them.
- **Onboard WS2812 RGB LED = GPIO21** (free for status-LED use).
- Buttons: RESET and BOOT.
- **Avoid** for I/O: 0 / 45 / 46 (strapping), 19 / 20 (native USB), 21 (LED),
  33–37 (PSRAM, not broken out anyway). GPIO3 is also a strapping pin — used
  here as an output, which normally works but is the first suspect if boot is
  flaky.

---

## 3. HUB75 data wiring (14 signals + ground)

The panel is 64 rows → **1/32 scan (HUB75E)**, so it needs the **E** address
line. Map (matches `include/config.h`):

| HUB75 signal | ESP32 pin | | HUB75 signal | ESP32 pin |
|---|---|---|---|---|
| R1 | GPIO1 | | C | GPIO9 |
| G1 | GPIO2 | | D | GPIO10 |
| B1 | GPIO3 | | E | GPIO11 |
| R2 | GPIO4 | | CLK | GPIO12 |
| G2 | GPIO5 | | LAT | GPIO13 |
| B2 | GPIO6 | | **OE** | **GPIO43** (the `TX` pad) |
| A | GPIO7 | | GND | any `GND` pin |
| B | GPIO8 | | | |

Notes:
- **OE is on GPIO43** (top-right `TX` pad). GPIO43/44 are UART0 but free,
  because logging goes over native USB (see build flags). Do **not** move OE to
  GPIO14 — it's on the bottom edge, not the side rows.
- The panel's 300 mm IDC→Dupont cable carries these signals **plus GND**; the
  Dupont leads push onto the MCU's soldered male header pins.
- **Logic level:** the ESP32 drives data at 3.3 V; HUB75 expects 5 V. Usually
  fine. If a future build shows flicker/wrong colors, add a level shifter
  (74HCT245).

---

## 4. Power (see `docs/power-plan.md` for the full reviewed writeup)

**Rule: feed the panel directly; the MCU only taps the rail. No panel current
through the board.**

```
[5V supply] ──> [DC-adapter screw terminals +/−]
                      │  (panel current stops here)
      ┌───────────────┼─────────────────────┐
      ▼               ▼                      ▼
 panel +5V (fork)  panel GND (fork)   branch to MCU: 5V pad + GND pad
```
- Supply must be **5 V, center-positive, 5.5×2.1 mm, ≥2 A** (panel worst case
  ~4 A at full white; a sparse/low-brightness build draws ~1–1.5 A).
- **Never use the `3V3` pad for power** — it's a 3.3 V output.
- **Programming:** MCU powered by laptop USB-C; keep only **one 5 V source at a
  time** — laptop OR supply, never both (their 5 V rails must not meet). GND may
  stay shared.

---

## 5. PlatformIO baseline

```ini
[env:s3mini]
platform = espressif32
board = lolin_s3_mini            ; same chip config as the S3-Zero
framework = arduino
monitor_speed = 115200
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1   ; serial logs over native USB
    -DARDUINO_USB_MODE=1
lib_deps =
    mrfaptastic/ESP32 HUB75 LED MATRIX PANEL DMA Display@^3.0.0
    ; + whatever your new software needs
```
Flash + monitor: `pio run -e s3mini -t upload -t monitor`

Panel init constants (from `config.h`): width 64, height 64, chain 1,
brightness 0–255 (90 is comfortable indoors; use ~20 while testing on USB).

---

## 6. Wi-Fi credentials pattern

`include/secrets.h` (gitignored) defines `WIFI_SSID` / `WIFI_PASS` behind
`#ifndef` guards; keep a committed `include/secrets.h.example`. Add a CLAUDE.md
rule so agents never read/edit the real `secrets.h`.

---

## Optional: Wi-Fi setup portal (no hardcoded credentials)

Instead of compile-time Wi-Fi creds, the device can configure itself from a
phone on first boot (captive portal, credentials stored in NVS). Reusable
pattern + minimal code in `docs/wifi-setup-portal.md`. Library:
`tzapu/WiFiManager@^2.0.17` (~110 KB flash). The onboard **BOOT button (GPIO0)**
makes a natural "re-open setup" trigger — read it *after* boot, not held through
reset.

## Files worth copying into the new repo

- `docs/esp32-s3-zero-board.png`, `docs/esp32-s3-zero-pinout.png` — board images
- `docs/power-plan.md` — full power writeup
- `docs/hardware-reference.md` — this file
- `docs/wifi-setup-portal.md` — optional phone-based Wi-Fi provisioning pattern
- `include/config.h` — the pin map + panel constants (Section 3 above)
- `include/secrets.h.example` — Wi-Fi template
- `platformio.ini` — board + build-flag baseline (Section 5)
