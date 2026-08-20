#pragma once

// ============================================================================
//  Golf Live Update — configuration
//  Everything you'd normally want to tweak lives in this file.
// ============================================================================

// ---------------------------------------------------------------------------
// Leaderboard behaviour
// ---------------------------------------------------------------------------

// ESPN's free (unofficial, no API key) PGA Tour scoreboard endpoint.
#define ESPN_SCOREBOARD_URL \
  "https://site.api.espn.com/apis/site/v2/sports/golf/pga/scoreboard"

// How often to refresh, and how quickly to retry after a failed fetch.
// When no tournament is live the board shows the upcoming event instead,
// which changes rarely — so it refreshes at the slower idle rate.
#define UPDATE_INTERVAL_MS      (5UL * 60UL * 1000UL)   // 5 minutes (live)
#define IDLE_UPDATE_INTERVAL_MS (30UL * 60UL * 1000UL)  // 30 minutes (no event)
#define RETRY_INTERVAL_MS       (30UL * 1000UL)         // 30 seconds

// Total golfers on the live board. Leaders fill the top; any tracked golfers
// sitting outside the top take the bottom rows (see MAX_PINNED_ROWS). 8 is the
// panel max. The leader block grows to keep the board full when fewer picks
// are pinned below.
#define BOARD_ROWS 8

// Debug: render a fake live leaderboard instead of fetching from ESPN. Handy
// for tweaking the live-scoreboard layout when no tournament is in progress.
// Set to 1, flash, and the board shows the fixture in loadDebugLeaderboard()
// (src/leaderboard.cpp) immediately — no network. Set back to 0 for normal use.
#define DEBUG_FAKE_LIVE 0

// Extra golfers to always show below the leaders (max 3 fit on the panel).
// Matched case-insensitively against the player's full name, with accents
// folded to ASCII — so "aberg" matches "Ludvig Åberg".
// A pinned golfer already among the leaders is not shown twice.
static const char* const PINNED_GOLFERS[] = {
    "Aberg",
    "Noren",   // matches "Alex Norén" (accents folded: é -> e)
};
static const size_t PINNED_GOLFER_COUNT =
    sizeof(PINNED_GOLFERS) / sizeof(PINNED_GOLFERS[0]);

// ---------------------------------------------------------------------------
// Wi-Fi setup portal
// ---------------------------------------------------------------------------

// On first boot (or when no saved network connects) the device starts its own
// Wi-Fi access point with this name and serves a captive-portal page to pick a
// network and enter the password. Credentials are stored on the device, so the
// real board needs no secrets.h. Tap the onboard BOOT button within ~3 s of
// power-on to re-open the portal and switch networks. (The Wokwi simulator
// skips all this and connects straight to Wokwi-GUEST.)
#define WIFI_SETUP_AP_NAME "GolfBoard-setup"

// ---------------------------------------------------------------------------
// Night schedule: power the display down overnight.
//
// The whole device deep-sleeps (panel dark, ESP32 at ~µA) and wakes by
// itself in the morning. Hours are local time in TIMEZONE_POSIX; the window
// may span midnight (start 23, end 7 = 23:00-07:00). Note that US West
// Coast tournaments run until ~02:00-03:00 Swedish time.
// ---------------------------------------------------------------------------

#define NIGHT_MODE_ENABLED true
#define NIGHT_START_HOUR 1  // display off at 01:00...
#define NIGHT_END_HOUR   7  // ...back on at 07:00

// POSIX timezone string. Default: Sweden (CET/CEST, DST handled).
#define TIMEZONE_POSIX "CET-1CEST,M3.5.0,M10.5.0/3"

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

#define PANEL_WIDTH   64
#define PANEL_HEIGHT  64
#define PANEL_CHAIN   1

// 0–255. P2 panels are BRIGHT — 255 at ~30 cm is uncomfortable, and drives
// the power draw toward the panel's 4 A max. 60–120 is plenty indoors.
#define PANEL_BRIGHTNESS 20   // low for USB-powered bench testing; raise to ~90 on the external supply

// Onboard WS2812 RGB LED (Waveshare S3-Zero, GPIO21). Unused by this project;
// blanked at boot so it doesn't glow a stray color.
#define ONBOARD_RGB_LED_PIN 21

// ---------------------------------------------------------------------------
// HUB75 pin mapping  (ESP32-S3 GPIO -> HUB75 input connector)
//
// This default map uses GPIO 1–13 + 43: exactly the pins on the two side
// header rows of the Waveshare ESP32-S3-Zero (verified against its pinout,
// docs/esp32-s3-zero-pinout.png). GPIO 43 is the pad silkscreened "TX" at the
// top of the right column. Left column = 5V,GND,3V3,GP1..GP6; right column =
// TX(43),RX(44),GP13..GP7. GPIO 14 sits on the bottom edge, so it is avoided.
// Any output-capable GPIO works on the S3; edit the numbers to rewire.
//
// AVOID: 0 / 45 / 46 (strapping), 19 / 20 (native USB — used for flashing
// and serial logs), 26–37 (internal flash / PSRAM on some modules), and on
// the S3-Zero: 21 (onboard WS2812 RGB LED).
// GPIO 43 / 44 are UART0 TX/RX but are free to use, since logging goes
// through native USB (ARDUINO_USB_CDC_ON_BOOT=1 in platformio.ini).
// ---------------------------------------------------------------------------

#ifdef WOKWI
// Wokwi simulator build (classic ESP32 devkit — the board whose HUB75
// emulation is proven). GPIO 1-13 collide with UART0/flash on that chip,
// so the simulator uses the pin map from Wokwi's own HUB75 examples.
// diagram.json mirrors this mapping.
#define HUB75_R1  25
#define HUB75_G1  26
#define HUB75_B1  27
#define HUB75_R2  14
#define HUB75_G2  12
#define HUB75_B2  13
#define HUB75_A   23
#define HUB75_B   19
#define HUB75_C   5
#define HUB75_D   17
#define HUB75_E   32
#define HUB75_CLK 16
#define HUB75_LAT 4
#define HUB75_OE  15
#else
#define HUB75_R1  1   // red,   top half
#define HUB75_G1  2   // green, top half
#define HUB75_B1  3   // blue,  top half
#define HUB75_R2  4   // red,   bottom half
#define HUB75_G2  5   // green, bottom half
#define HUB75_B2  6   // blue,  bottom half
#define HUB75_A   7   // row address bits: 64px panels use A..E (1/32 scan)
#define HUB75_B   8
#define HUB75_C   9
#define HUB75_D   10
#define HUB75_E   11
#define HUB75_CLK 12
#define HUB75_LAT 13
#define HUB75_OE  43   // the "TX" pad, top of the right column
#endif
