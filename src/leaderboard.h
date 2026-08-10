#pragma once

#include <Arduino.h>
#include "config.h"

// Up to 3 pinned golfers fit below the top-5 on a 64px-tall panel.
#define MAX_PINNED_ROWS 3

enum BoardMode : uint8_t {
  MODE_NONE,  // nothing on the calendar (deep off-season)
  MODE_LIVE,  // a tournament round is in progress -> leaderboard
  MODE_NEXT,  // between tournaments -> upcoming event + field status
};

struct GolferRow {
  char pos[5];    // "1", "T12", "-"
  char name[10];  // surname, ASCII-folded + uppercased, e.g. "ABERG"
  char score[6];  // "-14", "+2", "E"
  char thru[4];   // holes played: "12", "F" (finished), "-" (not started)
};

// Pinned golfer's status for the upcoming event.
struct NextGolfer {
  char name[10];
  char status[4];  // "IN" (in the field), "OUT" (not entered), "TBD"
};

struct Leaderboard {
  BoardMode mode = MODE_NONE;

  // MODE_LIVE
  char eventName[40];   // tournament name, ASCII-folded + uppercased
  char roundLabel[6];   // "R1".."R4"
  GolferRow leaders[LEADER_COUNT];
  uint8_t leaderCount = 0;
  GolferRow pinned[MAX_PINNED_ROWS];
  uint8_t pinnedCount = 0;

  // MODE_NEXT
  char nextName[40];    // upcoming tournament name
  char nextDates[16];   // "AUG 13-16", "AUG 30-SEP 2"
  NextGolfer nextGolfers[MAX_PINNED_ROWS];
  uint8_t nextGolferCount = 0;
};

// Fetches ESPN's scoreboard and fills `out`. Returns false on any
// network/parse error (in which case `out` is left untouched).
bool fetchLeaderboard(Leaderboard& out);
