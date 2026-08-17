// ============================================================================
//  Golf Live Update
//  Live PGA Tour leaderboard on a 64x64 HUB75 RGB LED matrix (ESP32-S3).
//
//  Data: ESPN's public scoreboard JSON, refreshed every 5 minutes.
//  See README.md for wiring and configuration.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <time.h>

#include "config.h"
#include "secrets.h"
#include "display.h"
#include "leaderboard.h"
#include "settings.h"

#ifndef WOKWI
#include <WiFiManager.h>   // captive-portal Wi-Fi setup on the real board
#include "webconfig.h"     // settings web page (golfboard.local)
#endif

static Leaderboard board;
static bool haveData = false;      // ever fetched successfully?
static bool lastFetchOk = false;
static uint32_t lastAttemptMs = 0;
static uint32_t nextDelayMs = 0;   // 0 = fetch immediately
static volatile bool refreshRequested = false;  // set by the web "Refresh now" button

// ---------------------------------------------------------------------------
// Night schedule. The system clock is set from the HTTP Date header of each
// fetch and survives deep sleep on the internal RTC — until the first fetch
// after a cold boot, the clock is invalid and night mode stays out of the way.
// ---------------------------------------------------------------------------

static bool clockValid() { return time(nullptr) > 1700000000; }

static bool isNight() {
  if (!settings.nightEnabled || !clockValid()) return false;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  if (settings.nightStart < settings.nightEnd) {
    return lt.tm_hour >= settings.nightStart && lt.tm_hour < settings.nightEnd;
  }
  return lt.tm_hour >= settings.nightStart ||
         lt.tm_hour < settings.nightEnd;  // spans midnight
}

static void sleepUntilMorning() {
  time_t now = time(nullptr);
  struct tm morning;
  localtime_r(&now, &morning);
  morning.tm_hour = settings.nightEnd;
  morning.tm_min = 0;
  morning.tm_sec = 0;
  morning.tm_isdst = -1;             // let mktime resolve DST
  time_t wake = mktime(&morning);    // local -> epoch (TZ is set)
  if (wake <= now) wake += 24 * 3600;

  // +60 s margin: the RTC's sleep timer drifts a little over hours, and
  // waking just after the boundary beats waking just before it (which
  // would only cost one extra sleep cycle anyway — setup() re-checks).
  uint64_t secs = (uint64_t)(wake - now) + 60;
  Serial.printf("[night] display off, sleeping %llu min\n", secs / 60);
  displayPowerOff();
  WiFi.disconnect(true);
  esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  esp_deep_sleep_start();  // wakes via reset into setup()
}

#ifndef WOKWI
// --- Real board: WiFiManager captive-portal setup -------------------------

// Shown on the panel when the setup access point comes up, so Wi-Fi can be
// configured with just a phone — no serial console needed.
static void onEnterSetupPortal(WiFiManager* /*wm*/) {
  Serial.printf("[wifi] setup AP '%s' up — browse to http://%s\n",
                WIFI_SETUP_AP_NAME, WiFi.softAPIP().toString().c_str());
  displayMessage("WIFI SETUP", "JOIN WIFI", WIFI_SETUP_AP_NAME);
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep hurts TLS reliability

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setAPCallback(onEnterSetupPortal);
  wm.setConfigPortalTimeout(0);  // keep the portal open until configured

  // Connects to the saved network, or opens the setup portal if there is none
  // or it can't connect. Networks are changed from the web page's "Reconfigure
  // Wi-Fi" button, so a boxed unit needs no physical controls.
  displayLoading("WIFI");
  if (!wm.autoConnect(WIFI_SETUP_AP_NAME)) {
    Serial.println("[wifi] setup failed/timed out — restarting");
    delay(1000);
    ESP.restart();
  }
  Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
}

#else
// --- Wokwi simulator: no phone for a portal, use build-flag creds ----------

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[wifi] connecting to %s", WIFI_SSID);
  displayLoading("WIFI");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
}
#endif

void setup() {
  Serial.begin(115200);

#ifndef WOKWI
  neopixelWrite(ONBOARD_RGB_LED_PIN, 0, 0, 0);  // blank the unused onboard WS2812
#endif

  settingsLoad();

  setenv("TZ", TIMEZONE_POSIX, 1);
  tzset();

  // Woke from deep sleep but morning hasn't actually arrived (clock kept
  // ticking through sleep): go straight back down without lighting the panel.
  if (isNight()) sleepUntilMorning();

  displayReleaseHolds();
  if (!displayInit()) {
    // Without a working panel there is nothing to show — log and halt.
    while (true) {
      Serial.println("[panel] DMA init failed — check wiring and pins in config.h");
      delay(5000);
    }
  }

  connectWiFi();
#ifndef WOKWI
  webconfigBegin(&board, &refreshRequested);
#endif
  displayLoading("FETCHING");
}

void loop() {
#ifndef WOKWI
  webconfigLoop();  // service the settings web page
#endif
  if (refreshRequested) {  // web "Refresh now": force an immediate fetch
    refreshRequested = false;
    nextDelayMs = 0;
  }

  // The clock ticked into the night window (or the first fetch of a late
  // cold boot just made the clock valid): power down until morning.
  if (isNight()) sleepUntilMorning();

  // Wi-Fi dropped: reconnect quietly, keep showing the last leaderboard.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] lost connection, reconnecting");
    if (!haveData) displayLoading("WIFI LOST");
    WiFi.disconnect();
    WiFi.reconnect();
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    if (WiFi.status() != WL_CONNECTED) {
      delay(5000);
      return;
    }
  }

  if (millis() - lastAttemptMs >= nextDelayMs || nextDelayMs == 0) {
    lastAttemptMs = millis();

#if DEBUG_FAKE_LIVE
    Serial.println("[debug] DEBUG_FAKE_LIVE: loading fake live leaderboard");
    loadDebugLeaderboard(board);
    lastFetchOk = true;
#else
    Serial.println("[espn] fetching leaderboard...");
    lastFetchOk = fetchLeaderboard(board);
#endif
    // The fetch synced the clock — if it's actually night, sleep before
    // lighting the panel (matters after a cold boot at 3 a.m.).
    if (isNight()) sleepUntilMorning();
    if (lastFetchOk) {
      haveData = true;
      // Live golf changes every few minutes; an upcoming-event screen doesn't.
      nextDelayMs = (board.mode == MODE_LIVE) ? UPDATE_INTERVAL_MS
                                              : IDLE_UPDATE_INTERVAL_MS;
      if (board.mode == MODE_LIVE) {
        Serial.printf("[espn] live: %s (%s), %d leaders, %d pinned\n",
                      board.eventName, board.roundLabel, board.leaderCount,
                      board.pinnedCount);
      } else {
        Serial.printf("[espn] next up: %s (%s), %d pinned golfers\n",
                      board.nextName, board.nextDates, board.nextGolferCount);
      }
    } else {
      nextDelayMs = RETRY_INTERVAL_MS;
      Serial.println("[espn] fetch failed, retrying soon");
    }

    if (haveData) {
      displayLeaderboard(board, lastFetchOk);
    } else {
      displayLoading("RETRYING");  // fetch failed and nothing to show yet
    }
  }

  delay(100);
}
