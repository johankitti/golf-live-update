# ⛳ Golf Live Update

**A live PGA Tour leaderboard on a 64×64 RGB LED matrix, powered by an ESP32-S3.**

Shows the tournament name, current round, the top 5, and your favourite golfers —
refreshed straight from ESPN every 5 minutes. No backend, no API key, no subscriptions.

```
┌────────────────────┐
│ WYNDHAM CHMP    R3 │  ← tournament + round
│ ────────────────── │
│ 1  SCHEFFLER -14 F │
│ 2  MCILROY   -11 15│  ← top 5 leaders
│ 3  RAHM      -10 12│     red = under par
│ 4  MORIKAWA   -8 F │     thru column: holes played
│ 5  FLEETWOOD  -7 16│
│ ────────────────── │
│ T23 ABERG     -2 9 │  ← your pinned golfers
└────────────────────┘  ● status dot: green = fresh
```

Between tournaments the board flips to a **"next up" screen** — the upcoming
event, its dates, and whether each of your golfers is in the field:

```
┌────────────────────┐
│ NEXT UP            │
│ ────────────────── │
│ FEDEX ST. JUDE     │
│ CHAMPIONSHIP       │
│ AUG 13-16          │
│ ────────────────── │
│ ABERG           IN │  ← green: confirmed in the field
│ NOREN          OUT │  ← orange: not entered
│ SCHEFFLER      TBD │  ← gray: field not published yet
└────────────────────┘
```

---

## 🛒 Bill of materials

| Part | Notes | Link |
|------|-------|------|
| 64×64 RGB LED matrix, P2, HUB75 | 128×128 mm, 1/32 scan | [Electrokit](https://www.electrokit.com/en/full-color-panel-2mm-rgb-led-matrix-64x64px-128x128mm-p2) |
| ESP32-S3 mini dev board | Rebranded [Waveshare ESP32-S3-Zero](https://www.waveshare.com/wiki/ESP32-S3-Zero): S3FH4R2, 4 MB flash / 2 MB PSRAM. Headers expose GPIO 1–13 + 43/44 — the default pin map matches them exactly | [Electrokit](https://www.electrokit.com/esp32-s3-utvecklingskort-mini-4mb-psram-2mb-med-headers) |
| 5 V power supply, ≥ 4 A | Powers the panel directly — **not** through the dev board | any quality 5 V/4 A PSU |
| Female–female dupont wires ×16 | Panel usually ships with an IDC data cable + power harness | — |

> **Why ESP32-S3 and not C5/C6?** HUB75 panels have no framebuffer — the MCU must
> re-stream the whole image 100+ times/second via DMA. The S3's LCD peripheral is
> what the battle-tested [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA)
> library is built on. The C5 has no official support in any HUB75 library (as of 2026).

---

## 🔌 Wiring

The panel's **data-in** connector (HUB75-E, 2×8 pins) hooks to the ESP32-S3 like this.
All pins are configurable in [`include/config.h`](include/config.h).

```
   HUB75-E connector (looking at the BACK of the panel, data-IN side)

        ┌───────────┐
    R1  │ ●1     2● │  G1
    B1  │ ●3     4● │  GND
    R2  │ ●5     6● │  G2
    B2  │ ●7     8● │  E
    A   │ ●9    10● │  B
    C   │ ●11   12● │  D
    CLK │ ●13   14● │  LAT
    OE  │ ●15   16● │  GND
        └───────────┘
```

| HUB75 signal | ESP32-S3 GPIO | | HUB75 signal | ESP32-S3 GPIO |
|:---:|:---:|---|:---:|:---:|
| R1 | 1 | | A | 7 |
| G1 | 2 | | B | 8 |
| B1 | 3 | | C | 9 |
| R2 | 4 | | D | 10 |
| G2 | 5 | | E | 11 |
| B2 | 6 | | CLK | 12 |
| LAT | 13 | | OE | 43 |
| GND | GND | | | |

**Power — read this once, save yourself a smoked board:**

- Feed the panel's screw/spade power terminals **directly from the 5 V PSU**.
  A 64×64 P2 panel can pull ~4 A at full white — far beyond what the dev board's
  5 V pin or USB port can supply.
- Connect **PSU GND ↔ panel GND ↔ ESP32 GND** (common ground, always).
- The dev board itself can run off USB, or off the same PSU's 5 V into its 5V pin.
- The default brightness is a conservative **20/255** — dim enough to be safe on
  USB while bench-testing. Turn it up (from the [web settings page](#-web-settings-page)
  or `PANEL_BRIGHTNESS` in `config.h`) once the panel is on the external supply;
  even at ~90/255 it stays well under 2 A.

---

## 🚀 Getting started

1. **Install [PlatformIO](https://platformio.org/)** (VS Code extension or `pipx install platformio`).

2. **Clone & configure:**

   ```bash
   git clone git@github.com:johankitti/golf-live-update.git
   cd golf-live-update
   cp include/secrets.h.example include/secrets.h   # needed to compile; no edit required
   ```

   > **Wi-Fi is configured on the device, not here.** On first boot the board
   > starts a setup hotspot — see [First-boot Wi-Fi setup](#-first-boot-wi-fi-setup)
   > below. `secrets.h` still has to exist for the build, but its placeholder
   > values are only used by the Wokwi simulator.

3. **Pick your golfers** in [`include/config.h`](include/config.h):

   ```cpp
   static const char* const PINNED_GOLFERS[] = {
       "Aberg",        // matches "Ludvig Åberg" (accents are folded)
       "Noren",        // up to 3 fit below the leaders
   };
   ```

   > This is just the **first-boot default** — once the board is running you can
   > change the tracked golfers (and brightness, and night hours) from the
   > [web settings page](#-web-settings-page) without reflashing.

4. **Build & flash** (board connected over USB):

   ```bash
   pio run -t upload          # ESP32-S3 mini (default)
   pio run -e devkitc -t upload   # full-size DevKitC instead
   pio device monitor         # watch the logs
   ```

That's it. The board connects to Wi-Fi, pulls the leaderboard, and refreshes
every 5 minutes (configurable via `UPDATE_INTERVAL_MS`).

---

## 📶 First-boot Wi-Fi setup

No credentials are compiled in — you set the network with your phone
(handled by [WiFiManager](https://github.com/tzapu/WiFiManager)):

1. On a fresh board the panel shows **`WIFI`** while it tries to connect. With no
   saved network it starts a Wi-Fi hotspot called **`GolfBoard-setup`** and the
   panel switches to **`WIFI SETUP / JOIN WIFI / GolfBoard-setup`**.
2. Join that hotspot on your phone — a captive-portal page opens automatically.
   Pick your Wi-Fi network, enter the password, save.
3. The board stores the credentials on-device (survives reflashing the app) and
   reconnects on its own. No `secrets.h` edit, no rebuild.

**To change networks later:** open the [settings page](#-web-settings-page) and
press **Reconfigure Wi-Fi** — the board wipes the saved network and re-opens the
`GolfBoard-setup` hotspot, so a boxed unit needs no physical buttons. The hotspot
name is `WIFI_SETUP_AP_NAME` in [`include/config.h`](include/config.h).

---

## 🌐 Web settings page

Once the board is on your network it serves a small settings page — no app, just a
browser. Open **[`http://golfboard.local/`](http://golfboard.local/)** (mDNS) or
the board's IP address (printed to the serial log, and shown on the page itself).

From there you can, without reflashing:

- **Brightness** — 0–255 slider, applied to the panel immediately on save.
- **Night mode** — toggle it and set the from/to hours (see below).
- **Tracked golfers** — up to 3 surnames to pin below the leaders.
- **Refresh now** — force an immediate re-fetch instead of waiting for the timer.
- **Restart** the board, or **Reconfigure Wi-Fi** (re-opens the setup hotspot).

The page also shows live status: the current event/round (or next event), the
board's IP, Wi-Fi signal strength, and uptime.

> **Where settings live.** The values in [`include/config.h`](include/config.h)
> (brightness, pinned golfers, night hours) are only the **first-boot defaults**.
> The moment you save from the web page they're written to the ESP32's NVS flash
> and become the source of truth — they persist across reboots *and* reflashes.
> Editing `config.h` afterward won't change a device that already has saved
> settings; use the web page, or erase NVS (`pio run -t erase`) to fall back to
> the config defaults.

---

## 🖥️ Try it without hardware

The whole project runs in the [Wokwi](https://wokwi.com) browser simulator — virtual
panel, virtual ESP32, and *real* internet: it fetches the actual live leaderboard
through Wokwi's simulated Wi-Fi (`Wokwi-GUEST`, baked into the `wokwi` build env).

```bash
pio run -e wokwi        # build the simulator firmware
```

Then either:

- **VS Code:** install the [Wokwi extension](https://marketplace.visualstudio.com/items?itemName=wokwi.wokwi-vscode),
  sign in (free), open this repo and run **"Wokwi: Start Simulator"** — it picks up
  `wokwi.toml` and `diagram.json` automatically.
- **Browser:** create a project at [wokwi.com](https://wokwi.com), paste in
  `diagram.json`, and upload the firmware.

Notes: the simulator uses a classic ESP32 devkit (Wokwi's HUB75 emulation is proven
on it) with its own pin map, selected automatically by the `WOKWI` build flag.
Simulated TLS is slow — the 1.3 MB ESPN download takes a while, which is your
chance to admire the loading animation.

---

## ⚙️ How it works

```mermaid
flowchart LR
    A["ESPN scoreboard API<br/>(~1.3 MB JSON)"] -->|"HTTPS, every 5 min"| B["ArduinoJson<br/>filtered stream parse<br/>(~50 KB kept, in PSRAM)"]
    B --> C["Leaderboard struct<br/>top 5 + pinned"]
    C --> D["Adafruit GFX renderer<br/>TomThumb 3×5 font"]
    D --> E["HUB75 DMA driver<br/>continuous panel refresh"]
```

- **Data source:** ESPN's public, keyless endpoint
  [`site.api.espn.com/.../golf/pga/scoreboard`](https://site.api.espn.com/apis/site/v2/sports/golf/pga/scoreboard).
- **The 1.3 MB problem:** the response is far bigger than the chip's RAM. The firmware
  streams it through [ArduinoJson's filter](https://arduinojson.org/v7/api/json/deserializejson/)
  so only ~a dozen fields per player are ever kept (in PSRAM).
- **Display:** [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA)
  refreshes the panel via DMA with zero CPU load between updates;
  [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) draws the text.
- **Names on a tiny font:** the LED font is ASCII-only, so names are accent-folded
  (`Åberg → ABERG`) before rendering.
- **Between tournaments:** the season calendar (embedded in the same API response)
  gives the next event and its dates. Each pinned golfer is then looked up in that
  event's published field → `IN` / `OUT`, or `TBD` until ESPN publishes entries
  (usually Mon–Tue of tournament week). ESPN has no public per-athlete schedule API,
  so field membership is the most truthful "is my golfer playing next week?" signal
  available. Idle refresh slows to 30 min to be polite.
- **No clock needed:** time comes from the HTTP `Date` response header,
  so there's no NTP dependency. It also sets the internal clock for the night schedule.
- **Night schedule:** between 01:00 and 07:00 (Swedish time — hours, on/off and the
  timezone come from `config.h`, and the hours and on/off toggle are also editable
  live from the [web settings page](#-web-settings-page)) the panel blanks and the
  whole device deep-sleeps at µA levels, waking
  itself in the morning. The HUB75 output-enable line is parked and held through
  sleep so the panel can't light stray pixels. Disable with `NIGHT_MODE_ENABLED false`.
  Heads-up: US West Coast tournaments run until ~02:00–03:00 Swedish time, so widen
  the window if you want to fall asleep to golf.
- **Loading state:** until the first data arrives, an animated boot screen (a golf
  ball rolling toward the pin) shows what the device is doing — `WIFI`, `FETCHING`,
  `RETRYING`. The animation runs on its own FreeRTOS task, so it keeps moving even
  while the network code blocks.
- **Resilience:** failed fetches retry after 30 s while the last good leaderboard stays
  up; the bottom-right dot turns red so you know it's stale. Wi-Fi drops reconnect
  automatically.

---

## 🧰 Troubleshooting

| Symptom | Fix |
|---------|-----|
| Panel black or garbage pixels | Uncomment `cfg.driver = HUB75_I2S_CFG::FM6126A;` in `src/display.cpp` — many panels use FM6126A driver chips |
| Colors swapped (red ↔ blue) | Swap the R1/B1 and R2/B2 pin numbers in `config.h` |
| Only half the panel lights up | E address line not connected/wrong pin — 64 px panels need A–E |
| Flicker or ghosting | Shorten wires; try `cfg.clkphase = false;` |
| `DMA init failed` in the serial log | A configured GPIO doesn't exist on your board — adjust `config.h` |
| No serial output | Logs go over native USB (`ARDUINO_USB_CDC_ON_BOOT=1`) — use the board's USB port, `pio device monitor` |

---

## 📄 License

MIT — do whatever you like, a ⭐ is always nice.
