#pragma once

#include <Arduino.h>
#include "config.h"

// Up to 3 pinned golfers fit below the top-5 on a 64px-tall panel.
#define MAX_PINNED_ROWS 3

struct GolferRow {
  char pos[5];    // "1", "T12", "-"
  char name[10];  // surname, ASCII-folded + uppercased, e.g. "ABERG"
  char score[6];  // "-14", "+2", "E"
  char thru[4];   // holes played: "12", "F" (finished), "-" (not started)
};

struct Leaderboard {
  char eventName[40];   // tournament name, ASCII-folded + uppercased
  char roundLabel[6];   // "R1".."R4", "F" (final), "PRE" (not started)
  GolferRow leaders[LEADER_COUNT];
  uint8_t leaderCount = 0;
  GolferRow pinned[MAX_PINNED_ROWS];
  uint8_t pinnedCount = 0;
  bool hasEvent = false;  // false = off-week, nothing scheduled
};

// Fetches ESPN's scoreboard and fills `out`. Returns false on any
// network/parse error (in which case `out` is left untouched).
bool fetchLeaderboard(Leaderboard& out);
