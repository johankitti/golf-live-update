#include "leaderboard.h"
#include "settings.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include <time.h>

// ---------------------------------------------------------------------------
// PSRAM allocator for ArduinoJson: the filtered document (~40-80 KB for a
// 150-player field) lands in external PSRAM, keeping internal SRAM free for
// the TLS buffers and the panel's DMA framebuffer.
// ---------------------------------------------------------------------------
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    if (psramFound()) return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return malloc(size);
  }
  void deallocate(void* ptr) override { heap_caps_free(ptr); }
  void* reallocate(void* ptr, size_t newSize) override {
    if (psramFound()) return heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM);
    return realloc(ptr, newSize);
  }
};

// ---------------------------------------------------------------------------
// Text helpers: the LED font is ASCII-only, so "Ludvig Åberg" must become
// "ABERG" before it can be drawn.
// ---------------------------------------------------------------------------

// Folds UTF-8 Latin-1 accents to plain ASCII (Å->A, é->e, ø->o, ß->s ...).
// Characters outside that range (rare on the PGA Tour) are dropped.
static void asciiFold(const char* src, char* dst, size_t dstSize) {
  size_t o = 0;
  const uint8_t* s = (const uint8_t*)src;
  while (*s && o < dstSize - 1) {
    uint8_t b = *s;
    if (b < 0x80) {
      dst[o++] = (char)b;
      s++;
    } else if (b == 0xC3 && s[1]) {
      uint8_t c = s[1];
      char r = 0;
      if (c >= 0x80 && c <= 0x86)      r = 'A';
      else if (c == 0x87)              r = 'C';
      else if (c >= 0x88 && c <= 0x8B) r = 'E';
      else if (c >= 0x8C && c <= 0x8F) r = 'I';
      else if (c == 0x90)              r = 'D';
      else if (c == 0x91)              r = 'N';
      else if ((c >= 0x92 && c <= 0x96) || c == 0x98) r = 'O';
      else if (c >= 0x99 && c <= 0x9C) r = 'U';
      else if (c == 0x9D)              r = 'Y';
      else if (c == 0x9F)              r = 's';
      else if (c >= 0xA0 && c <= 0xA6) r = 'a';
      else if (c == 0xA7)              r = 'c';
      else if (c >= 0xA8 && c <= 0xAB) r = 'e';
      else if (c >= 0xAC && c <= 0xAF) r = 'i';
      else if (c == 0xB0)              r = 'd';
      else if (c == 0xB1)              r = 'n';
      else if ((c >= 0xB2 && c <= 0xB6) || c == 0xB8) r = 'o';
      else if (c >= 0xB9 && c <= 0xBC) r = 'u';
      else if (c == 0xBD || c == 0xBF) r = 'y';
      if (r) dst[o++] = r;
      s += 2;
    } else {
      s++;
      while ((*s & 0xC0) == 0x80) s++;  // skip continuation bytes
    }
  }
  dst[o] = 0;
}

static void toUpperInPlace(char* s) {
  for (; *s; s++) *s = toupper((unsigned char)*s);
}

// "Ludvig Åberg" -> "ABERG": last name only, folded and uppercased.
static void surnameOf(const char* fullName, char* dst, size_t dstSize) {
  char folded[32];
  asciiFold(fullName, folded, sizeof(folded));
  const char* last = strrchr(folded, ' ');
  strlcpy(dst, last ? last + 1 : folded, dstSize);
  toUpperInPlace(dst);
}

// Case-insensitive substring match of one config pattern against the
// folded full name, so "aberg" matches "Ludvig Åberg".
static bool nameMatchesPattern(const char* fullName, const char* pattern) {
  char folded[48], pat[32];
  asciiFold(fullName, folded, sizeof(folded));
  toUpperInPlace(folded);
  strlcpy(pat, pattern, sizeof(pat));
  toUpperInPlace(pat);
  return strstr(folded, pat) != nullptr;
}

static bool matchesAnyPinned(const char* fullName) {
  for (uint8_t i = 0; i < settings.pinnedCount; i++) {
    if (nameMatchesPattern(fullName, settings.pinned[i])) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Date helpers. "Now" comes from the HTTP Date header of each response;
// it also sets the system clock, which the night schedule relies on.
//
// All parsing converts to epoch WITHOUT mktime(): mktime interprets struct
// tm in the device's local timezone, and main.cpp sets TZ to Sweden for the
// night schedule — these timestamps are UTC and must stay that way.
// ---------------------------------------------------------------------------

// Civil date -> UTC epoch (Howard Hinnant's days-from-civil algorithm).
static time_t utcEpoch(int y, int m, int d, int hh, int mm, int ss) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  long days = (long)era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + hh * 3600 + mm * 60 + ss;
}

static const char* const MONTHS_UP[12] = {"JAN", "FEB", "MAR", "APR",
                                          "MAY", "JUN", "JUL", "AUG",
                                          "SEP", "OCT", "NOV", "DEC"};

static int monthIndex(const char* mon3) {
  static const char* names = "janfebmaraprmayjunjulaugsepoctnovdec";
  for (int i = 0; i < 12; i++) {
    if (strncasecmp(mon3, names + i * 3, 3) == 0) return i;
  }
  return -1;
}

// "Sun, 10 Aug 2026 15:49:37 GMT" -> UTC epoch, 0 on failure.
static time_t parseHttpDate(const char* s) {
  char mon[4] = {0};
  int d, y, hh, mm, ss;
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6)
    return 0;
  int m = monthIndex(mon);
  if (m < 0) return 0;
  return utcEpoch(y, m + 1, d, hh, mm, ss);
}

// "2026-08-13T07:00Z" -> UTC epoch at ~noon that day (day resolution is all
// the calendar logic needs), 0 on failure.
static time_t parseIsoDate(const char* s) {
  int y, m, d;
  if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 0;
  return utcEpoch(y, m, d, 12, 0, 0);
}

// ("2026-08-13...", "2026-08-16...") -> "AUG 13-16"; across a month
// boundary -> "AUG 30-SEP 2".
static void formatDateRange(const char* startIso, const char* endIso,
                            char* dst, size_t dstSize) {
  int y1, m1, d1, y2, m2, d2;
  if (sscanf(startIso, "%d-%d-%d", &y1, &m1, &d1) != 3) {
    strlcpy(dst, "", dstSize);
    return;
  }
  if (sscanf(endIso, "%d-%d-%d", &y2, &m2, &d2) != 3) {
    snprintf(dst, dstSize, "%s %d", MONTHS_UP[m1 - 1], d1);
    return;
  }
  if (m1 == m2) {
    snprintf(dst, dstSize, "%s %d-%d", MONTHS_UP[m1 - 1], d1, d2);
  } else {
    snprintf(dst, dstSize, "%s %d-%s %d", MONTHS_UP[m1 - 1], d1,
             MONTHS_UP[m2 - 1], d2);
  }
}

// ---------------------------------------------------------------------------
// HTTP: fetch a URL and filter-parse it. Optionally reports the server's
// clock (from the Date header) via nowUtc.
// ---------------------------------------------------------------------------

static bool getJson(const char* url, JsonDocument& doc,
                    const JsonDocument& filter, time_t* nowUtc) {
  WiFiClientSecure client;
  client.setInsecure();  // hobby display: skip cert validation so the board
                         // keeps working when ESPN rotates certificates

  HTTPClient http;
  // HTTP/1.0 disables chunked transfer encoding — required so ArduinoJson
  // can parse the response stream directly.
  http.useHTTP10(true);
  http.setTimeout(20000);
  http.setConnectTimeout(10000);
  if (!http.begin(client, url)) return false;

  static const char* headerKeys[] = {"Date"};
  if (nowUtc) http.collectHeaders(headerKeys, 1);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[espn] HTTP %d\n", code);
    http.end();
    return false;
  }
  if (nowUtc) {
    *nowUtc = parseHttpDate(http.header("Date").c_str());
    // Keep the system clock in sync — the night schedule depends on it,
    // and it survives deep sleep on the ESP32's internal RTC.
    if (*nowUtc > 1700000000) {
      struct timeval tv = {.tv_sec = *nowUtc, .tv_usec = 0};
      settimeofday(&tv, nullptr);
    }
  }

  // Buffer the whole (~0.5 MB) body into PSRAM, then parse from RAM. Parsing
  // straight off the TLS stream is unreliable at this size: a slow filtered
  // parse lets the connection drop mid-body (JSON error: IncompleteInput). A
  // tight read loop can't be outrun by the parser.
  WiFiClient* stream = http.getStreamPtr();
  size_t cap = 128 * 1024, len = 0;
  char* body = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!body) { http.end(); return false; }
  uint32_t lastRx = millis();
  while (http.connected() || stream->available()) {
    size_t avail = stream->available();
    if (avail) {
      if (len + avail > cap) {
        while (len + avail > cap) cap += 128 * 1024;
        char* nb = (char*)heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM);
        if (!nb) { heap_caps_free(body); http.end(); return false; }
        body = nb;
      }
      len += stream->readBytes(body + len, avail);
      lastRx = millis();
    } else if (millis() - lastRx > 20000) {
      break;  // stalled: give up, retry later
    } else {
      delay(2);
    }
  }
  http.end();

  // ESPN nests deeper than ArduinoJson's default limit of 10 (events >
  // competitions > competitors > ...); raise the ceiling (costs stack only for
  // the actual depth, ~15).
  DeserializationError err =
      deserializeJson(doc, body, len, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(30));
  heap_caps_free(body);
  if (err) {
    Serial.printf("[espn] JSON error: %s\n", err.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// MODE_LIVE: leaderboard rows
// ---------------------------------------------------------------------------

static void fillRow(GolferRow& row, JsonObjectConst c, const char* compState,
                    int period) {
  // Position: live events carry "T5"-style strings; finished events don't,
  // so fall back to the competitor's sort order.
  const char* posDisp = c["status"]["position"]["displayName"];
  if (posDisp && *posDisp) {
    strlcpy(row.pos, posDisp, sizeof(row.pos));
  } else {
    snprintf(row.pos, sizeof(row.pos), "%d", (int)(c["order"] | 0));
  }

  surnameOf(c["athlete"]["displayName"] | "?", row.name, sizeof(row.name));

  // Total score to par: ESPN sends a display string ("-14", "E") but be
  // defensive about it arriving as a number.
  JsonVariantConst sv = c["score"];
  if (sv.is<const char*>()) {
    strlcpy(row.score, sv.as<const char*>(), sizeof(row.score));
  } else if (!sv.isNull()) {
    int v = sv.as<int>();
    if (v == 0) strlcpy(row.score, "E", sizeof(row.score));
    else snprintf(row.score, sizeof(row.score), "%+d", v);
  } else {
    strlcpy(row.score, "-", sizeof(row.score));
  }

  // This round's score to par + holes played come from the current round's
  // linescore. competitor.status is null on this feed, so the per-hole array
  // length is the reliable "thru".
  strlcpy(row.today, "-", sizeof(row.today));
  strlcpy(row.thru, "-", sizeof(row.thru));
  for (JsonObjectConst ls : c["linescores"].as<JsonArrayConst>()) {
    if ((int)(ls["period"] | 0) != period) continue;
    const char* tv = ls["displayValue"];
    if (tv && *tv) strlcpy(row.today, tv, sizeof(row.today));
    int holes = ls["linescores"].as<JsonArrayConst>().size();
    if (strcmp(compState, "post") == 0 || holes >= 18) {
      strlcpy(row.thru, "F", sizeof(row.thru));
    } else if (holes > 0) {
      snprintf(row.thru, sizeof(row.thru), "%d", holes);
    }
    break;
  }
}

static void fillLive(Leaderboard& lb, JsonObjectConst event) {
  lb.mode = MODE_LIVE;

  const char* rawName = event["shortName"] | (const char*)(event["name"] | "PGA TOUR");
  asciiFold(rawName, lb.eventName, sizeof(lb.eventName));
  toUpperInPlace(lb.eventName);

  JsonObjectConst comp = event["competitions"][0];
  const char* compState = comp["status"]["type"]["state"] | "in";
  int period = comp["status"]["period"] | 1;
  snprintf(lb.roundLabel, sizeof(lb.roundLabel), "R%d", period > 0 ? period : 1);

  // Competitors arrive sorted by leaderboard order: the first LEADER_COUNT
  // are the leaders; pinned golfers are picked up wherever they sit.
  // A pinned golfer inside the leaders is already visible and is not duplicated.
  const uint8_t pinnedTarget =
      min((size_t)MAX_PINNED_ROWS, (size_t)settings.pinnedCount);
  for (JsonObjectConst c : comp["competitors"].as<JsonArrayConst>()) {
    bool isPinned = matchesAnyPinned(c["athlete"]["displayName"] | "");
    if (lb.leaderCount < LEADER_COUNT) {
      GolferRow& r = lb.leaders[lb.leaderCount++];
      fillRow(r, c, compState, period);
      r.selected = isPinned;  // a pick that made the top rows stays highlighted
    } else if (lb.pinnedCount < pinnedTarget && isPinned) {
      GolferRow& r = lb.pinned[lb.pinnedCount++];
      fillRow(r, c, compState, period);
      r.selected = true;
    } else if (lb.leaderCount >= LEADER_COUNT &&
               lb.pinnedCount >= pinnedTarget) {
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// MODE_NEXT: upcoming tournament + per-golfer field status.
//
// ESPN has no public "athlete schedule" API, so "which event does golfer X
// play next" is answered as: is X in the field of the tour's next event?
// The field is usually published Mon/Tue of tournament week — until then
// every pinned golfer shows TBD.
// ---------------------------------------------------------------------------

static void fillNext(Leaderboard& lb, const JsonDocument& doc, time_t now,
                     time_t completedStart,
                     ArduinoJson::Allocator* allocator) {
  lb.mode = MODE_NEXT;

  // The scoreboard's Date header should always parse; if it somehow didn't,
  // approximate "now" with the currently listed event's date.
  if (now == 0) now = parseIsoDate(doc["events"][0]["date"] | "");

  // Pick the current-or-next tournament from the season calendar.
  //
  // `completedStart` is the start date of the event ESPN currently reports as
  // finished ("post"), or 0 if none. A tournament's calendar endDate is a UTC
  // timestamp that rolls a day past the final round, and parseIsoDate keeps
  // only the date part — so a purely date-based "has it ended?" test lingers a
  // day or two after the trophy is lifted. We instead trust ESPN's own state
  // and skip the just-finished event (and anything before it) outright. The
  // endDate test stays as a fallback for when the feed has no completed event.
  const char* startIso = nullptr;
  for (JsonObjectConst c : doc["leagues"][0]["calendar"].as<JsonArrayConst>()) {
    time_t start = parseIsoDate(c["startDate"] | "");
    if (completedStart != 0 && start != 0 && start <= completedStart) continue;
    time_t end = parseIsoDate(c["endDate"] | "");
    if (end == 0 || end + 86400 <= now) continue;
    asciiFold(c["label"] | "PGA TOUR", lb.nextName, sizeof(lb.nextName));
    toUpperInPlace(lb.nextName);
    startIso = c["startDate"] | "";
    formatDateRange(startIso, c["endDate"] | "", lb.nextDates,
                    sizeof(lb.nextDates));
    break;
  }
  if (!startIso || !*startIso) {
    lb.mode = MODE_NONE;  // calendar exhausted: season is over
    return;
  }

  // Pinned golfers start as TBD, named after their config pattern.
  lb.nextGolferCount = min((size_t)MAX_PINNED_ROWS, (size_t)settings.pinnedCount);
  for (uint8_t i = 0; i < lb.nextGolferCount; i++) {
    strlcpy(lb.nextGolfers[i].name, settings.pinned[i],
            sizeof(lb.nextGolfers[i].name));
    toUpperInPlace(lb.nextGolfers[i].name);
    strlcpy(lb.nextGolfers[i].status, "TBD", sizeof(lb.nextGolfers[i].status));
  }
  if (lb.nextGolferCount == 0) return;

  // Ask the scoreboard for the next event's start date — once entries are
  // published its competitor list is the tournament field.
  int y, m, d;
  if (sscanf(startIso, "%d-%d-%d", &y, &m, &d) != 3) return;
  char url[160];
  snprintf(url, sizeof(url), "%s?dates=%04d%02d%02d", ESPN_SCOREBOARD_URL, y, m, d);

  JsonDocument filter;
  JsonObject fEvent = filter["events"].add<JsonObject>();
  JsonObject fc = fEvent["competitions"].add<JsonObject>()["competitors"]
                      .add<JsonObject>();
  fc["athlete"]["displayName"] = true;

  JsonDocument fieldDoc(allocator);
  if (!getJson(url, fieldDoc, filter, nullptr)) return;  // stay TBD

  JsonArrayConst field =
      fieldDoc["events"][0]["competitions"][0]["competitors"].as<JsonArrayConst>();
  if (field.size() == 0) return;  // field not published yet: stay TBD

  for (uint8_t i = 0; i < lb.nextGolferCount; i++) {
    strlcpy(lb.nextGolfers[i].status, "OUT", sizeof(lb.nextGolfers[i].status));
    for (JsonObjectConst c : field) {
      const char* fullName = c["athlete"]["displayName"] | "";
      if (nameMatchesPattern(fullName, settings.pinned[i])) {
        surnameOf(fullName, lb.nextGolfers[i].name, sizeof(lb.nextGolfers[i].name));
        strlcpy(lb.nextGolfers[i].status, "IN", sizeof(lb.nextGolfers[i].status));
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool fetchLeaderboard(Leaderboard& out) {
  // Filter: of the ~1.3 MB response, keep only these fields (~50 KB).
  JsonDocument filter;
  JsonObject fEvent = filter["events"].add<JsonObject>();
  fEvent["shortName"] = true;
  fEvent["name"] = true;
  fEvent["date"] = true;
  JsonObject fComp = fEvent["competitions"].add<JsonObject>();
  fComp["status"]["period"] = true;
  fComp["status"]["type"]["state"] = true;
  JsonObject fc = fComp["competitors"].add<JsonObject>();
  fc["order"] = true;
  fc["score"] = true;
  fc["athlete"]["displayName"] = true;
  fc["status"]["thru"] = true;
  fc["status"]["type"]["state"] = true;
  fc["status"]["position"]["displayName"] = true;
  // Per-round linescores: today's score to par (displayValue), and holes
  // played via the nested per-hole array (competitor.status is null on this
  // feed, so the array length is the only reliable "thru"). The whole body is
  // buffered before parsing (see getJson), so this heavier parse is safe.
  JsonObject fLine = fc["linescores"].add<JsonObject>();
  fLine["period"] = true;
  fLine["displayValue"] = true;
  fLine["linescores"].add<JsonObject>()["period"] = true;  // per-hole, for counting thru
  // Season calendar, for the "next tournament" screen.
  JsonObject fCal = filter["leagues"].add<JsonObject>()["calendar"].add<JsonObject>();
  fCal["label"] = true;
  fCal["startDate"] = true;
  fCal["endDate"] = true;

  SpiRamAllocator allocator;
  JsonDocument doc(&allocator);
  time_t now = 0;
  if (!getJson(ESPN_SCOREBOARD_URL, doc, filter, &now)) return false;

  // A tournament round in progress wins; otherwise show what's next. While
  // scanning, note the latest event ESPN reports as finished ("post") so the
  // "next" screen can skip a tournament that just ended (see fillNext).
  JsonObjectConst liveEvent;
  time_t completedStart = 0;
  for (JsonObjectConst ev : doc["events"].as<JsonArrayConst>()) {
    const char* st = ev["competitions"][0]["status"]["type"]["state"] | "";
    if (strcmp(st, "in") == 0) {
      liveEvent = ev;
      break;
    }
    if (strcmp(st, "post") == 0) {
      time_t s = parseIsoDate(ev["date"] | "");
      if (s > completedStart) completedStart = s;
    }
  }

  Leaderboard lb;  // build into a temp so `out` stays intact on failure
  if (!liveEvent.isNull()) {
    fillLive(lb, liveEvent);
  } else {
    fillNext(lb, doc, now, completedStart, &allocator);
  }

  out = lb;
  return true;
}

// ---------------------------------------------------------------------------
// Debug fixture: a synthetic live leaderboard for iterating on the display
// without waiting for a real tournament. Enabled via DEBUG_FAKE_LIVE in
// config.h; main.cpp loads this instead of fetching. Edit the rows below to
// try different names, scores and "thru" states against renderLive/drawRow.
// ---------------------------------------------------------------------------

static void setRow(GolferRow& r, const char* pos, const char* name,
                   const char* today, const char* score, const char* thru) {
  strlcpy(r.pos, pos, sizeof(r.pos));
  strlcpy(r.name, name, sizeof(r.name));
  strlcpy(r.today, today, sizeof(r.today));
  strlcpy(r.score, score, sizeof(r.score));
  strlcpy(r.thru, thru, sizeof(r.thru));
}

void loadDebugLeaderboard(Leaderboard& out) {
  Leaderboard lb;
  lb.mode = MODE_LIVE;
  strlcpy(lb.eventName, "BMW CHAMPIONSHIP", sizeof(lb.eventName));
  strlcpy(lb.roundLabel, "R2", sizeof(lb.roundLabel));

  // Top 6 — mixes solo/tied ranks, under/over/even scores, and every "thru"
  // state (finished, mid-round, not yet started), plus a highlighted pick.
  //          pos    name         today  total  thru
  setRow(lb.leaders[0], "1",   "SCHEFFLER", "-5", "-15", "F");
  setRow(lb.leaders[1], "T2",  "MCILROY",   "-3", "-12", "F");
  setRow(lb.leaders[2], "T2",  "HENLEY",    "-4", "-12", "16");
  setRow(lb.leaders[3], "4",   "MORIKAWA",  "-4", "-10", "12");
  setRow(lb.leaders[4], "5",   "ABERG",     "-3", "-9",  "13");
  lb.leaders[4].selected = true;  // Åberg is a pick -> stays highlighted up here
  setRow(lb.leaders[5], "6",   "FLEETWOOD", "-1", "-8",  "7");
  lb.leaderCount = 6;

  // Pinned golfers below the divider. Åberg isn't repeated here: he's already
  // among the leaders above, the same de-dup the live feed does when a pinned
  // golfer sits inside the top rows.
  setRow(lb.pinned[0], "T21", "NOREN", "+2", "-1", "9");
  lb.pinned[0].selected = true;
  lb.pinnedCount = 1;

  out = lb;
}
