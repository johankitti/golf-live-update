#pragma once

#include "leaderboard.h"

// Settings web page + mDNS (golfboard.local), served continuously while the
// board is connected to Wi-Fi. Call webconfigBegin() once after Wi-Fi is up,
// and webconfigLoop() often from loop() to service requests.
//
//   board       - live leaderboard, shown in the status panel (may be null)
//   refreshFlag - set true when the user clicks "Refresh now"
void webconfigBegin(const Leaderboard* board, volatile bool* refreshFlag);
void webconfigLoop();
