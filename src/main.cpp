// ============================================================================
//  Golf Live Update
//  Live PGA Tour leaderboard on a 64x64 HUB75 RGB LED matrix (ESP32-S3).
//
//  Data: ESPN's public scoreboard JSON, refreshed every 5 minutes.
//  See README.md for wiring and configuration.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "secrets.h"
#include "display.h"
#include "leaderboard.h"

static Leaderboard board;
static bool haveData = false;      // ever fetched successfully?
static bool lastFetchOk = false;
static uint32_t lastAttemptMs = 0;
static uint32_t nextDelayMs = 0;   // 0 = fetch immediately

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep hurts TLS reliability
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.printf("[wifi] connecting to %s", WIFI_SSID);
  displayMessage("GOLF", "LEADERBOARD", "wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);

  if (!displayInit()) {
    // Without a working panel there is nothing to show — log and halt.
    while (true) {
      Serial.println("[panel] DMA init failed — check wiring and pins in config.h");
      delay(5000);
    }
  }

  connectWiFi();
  displayMessage("GOLF", "LEADERBOARD", "loading...");
}

void loop() {
  // Wi-Fi dropped: reconnect quietly, keep showing the last leaderboard.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] lost connection, reconnecting");
    if (!haveData) displayMessage("GOLF", "LEADERBOARD", "wifi...");
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

    Serial.println("[espn] fetching leaderboard...");
    lastFetchOk = fetchLeaderboard(board);
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
      displayMessage("GOLF", "LEADERBOARD", "no data yet");
    }
  }

  delay(100);
}
