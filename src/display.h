#pragma once

#include "leaderboard.h"

// Initialises the HUB75 panel driver. Returns false if DMA setup fails.
bool displayInit();

// Full-screen status message (boot, Wi-Fi, errors). Up to 3 centered lines.
void displayMessage(const char* l1, const char* l2 = nullptr,
                    const char* l3 = nullptr);

// Draws the leaderboard. `fetchOk` controls the little status dot in the
// bottom-right corner: green = data is fresh, red = last refresh failed
// (the board keeps showing the previous data).
void displayLeaderboard(const Leaderboard& lb, bool fetchOk);
