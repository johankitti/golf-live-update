#pragma once

#include "leaderboard.h"

// Initialises the HUB75 panel driver. Returns false if DMA setup fails.
bool displayInit();

// Sets panel brightness (0-255) live, e.g. from the web settings page.
void displaySetBrightness(uint8_t b);

// Full-screen status message (boot, Wi-Fi, errors). Up to 3 centered lines.
void displayMessage(const char* l1, const char* l2 = nullptr,
                    const char* l3 = nullptr);

// Draws the leaderboard. `fetchOk` is currently unused (it used to drive a
// small fresh/stale status dot in the corner, since removed); kept in the
// signature so a staleness indicator can be reintroduced without churn.
void displayLeaderboard(const Leaderboard& lb, bool fetchOk);

// Animated loading screen (rolling golf ball) with a status line, e.g.
// "WIFI" / "FETCHING". The animation runs on its own FreeRTOS task, so it
// keeps moving while the main task blocks on the network. Calling it again
// just updates the status line. Any full-screen render stops it.
void displayLoading(const char* status);
void displayLoadingStop();

// Night mode: blanks the panel and parks/holds the OE line so the matrix
// stays dark through deep sleep (floating HUB75 inputs light random rows).
// Safe to call even before displayInit().
void displayPowerOff();

// Releases the deep-sleep pin holds. Call once at boot, before displayInit().
void displayReleaseHolds();
