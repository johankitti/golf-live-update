#include "leaderboard.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

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

// Case-insensitive substring match against the folded full name, so a
// config entry "aberg" matches "Ludvig Åberg".
static bool matchesPinned(const char* fullName) {
  char folded[48];
  asciiFold(fullName, folded, sizeof(folded));
  toUpperInPlace(folded);
  for (size_t i = 0; i < PINNED_GOLFER_COUNT; i++) {
    char pat[32];
    strlcpy(pat, PINNED_GOLFERS[i], sizeof(pat));
    toUpperInPlace(pat);
    if (strstr(folded, pat)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Row extraction
// ---------------------------------------------------------------------------

static void fillRow(GolferRow& row, JsonObjectConst c, const char* compState) {
  // Position: live events carry "T5"-style strings; finished events don't,
  // so fall back to the competitor's sort order.
  const char* posDisp = c["status"]["position"]["displayName"];
  if (posDisp && *posDisp) {
    strlcpy(row.pos, posDisp, sizeof(row.pos));
  } else {
    snprintf(row.pos, sizeof(row.pos), "%d", (int)(c["order"] | 0));
  }

  surnameOf(c["athlete"]["displayName"] | "?", row.name, sizeof(row.name));

  // Score to par: ESPN sends a display string ("-14", "E") but be defensive
  // about it arriving as a number.
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

  // Holes played this round.
  if (strcmp(compState, "post") == 0) {
    strlcpy(row.thru, "F", sizeof(row.thru));
  } else {
    int thru = c["status"]["thru"] | 0;
    const char* pState = c["status"]["type"]["state"] | "";
    if (thru >= 18 || strcmp(pState, "post") == 0) {
      strlcpy(row.thru, "F", sizeof(row.thru));
    } else if (thru > 0) {
      snprintf(row.thru, sizeof(row.thru), "%d", thru);
    } else {
      strlcpy(row.thru, "-", sizeof(row.thru));
    }
  }
}

// ---------------------------------------------------------------------------
// Fetch + parse
// ---------------------------------------------------------------------------

bool fetchLeaderboard(Leaderboard& out) {
  WiFiClientSecure client;
  client.setInsecure();  // hobby display: skip cert validation so the board
                         // keeps working when ESPN rotates certificates

  HTTPClient http;
  // HTTP/1.0 disables chunked transfer encoding — required so ArduinoJson
  // can parse the response stream directly.
  http.useHTTP10(true);
  http.setTimeout(20000);
  http.setConnectTimeout(10000);
  if (!http.begin(client, ESPN_SCOREBOARD_URL)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[espn] HTTP %d\n", code);
    http.end();
    return false;
  }

  // Filter: of the ~1.3 MB response, keep only these fields (~50 KB).
  JsonDocument filter;
  JsonObject fEvent = filter["events"].add<JsonObject>();
  fEvent["shortName"] = true;
  fEvent["name"] = true;
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

  SpiRamAllocator allocator;
  JsonDocument doc(&allocator);
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("[espn] JSON error: %s\n", err.c_str());
    return false;
  }

  // Pick the event: prefer one that is live right now (there are sometimes
  // two in the same week), otherwise take the first listed.
  JsonArrayConst events = doc["events"];
  JsonObjectConst event;
  for (JsonObjectConst ev : events) {
    const char* st = ev["competitions"][0]["status"]["type"]["state"] | "";
    if (strcmp(st, "in") == 0) { event = ev; break; }
    if (event.isNull()) event = ev;
  }

  Leaderboard lb;  // build into a temp so `out` stays intact on failure
  if (event.isNull()) {
    lb.hasEvent = false;
    strlcpy(lb.eventName, "NO PGA EVENT", sizeof(lb.eventName));
    out = lb;
    return true;
  }
  lb.hasEvent = true;

  const char* rawName = event["shortName"] | (const char*)(event["name"] | "PGA TOUR");
  asciiFold(rawName, lb.eventName, sizeof(lb.eventName));
  toUpperInPlace(lb.eventName);

  JsonObjectConst comp = event["competitions"][0];
  const char* compState = comp["status"]["type"]["state"] | "in";
  int period = comp["status"]["period"] | 0;
  if (strcmp(compState, "post") == 0) {
    strlcpy(lb.roundLabel, "F", sizeof(lb.roundLabel));
  } else if (strcmp(compState, "pre") == 0) {
    strlcpy(lb.roundLabel, "PRE", sizeof(lb.roundLabel));
  } else {
    snprintf(lb.roundLabel, sizeof(lb.roundLabel), "R%d", period > 0 ? period : 1);
  }

  // Competitors arrive sorted by leaderboard order: the first LEADER_COUNT
  // are the leaders; pinned golfers are picked up wherever they sit.
  // A pinned golfer inside the top 5 is already visible and is not duplicated.
  const uint8_t pinnedTarget =
      min((size_t)MAX_PINNED_ROWS, PINNED_GOLFER_COUNT);
  for (JsonObjectConst c : comp["competitors"].as<JsonArrayConst>()) {
    if (lb.leaderCount < LEADER_COUNT) {
      fillRow(lb.leaders[lb.leaderCount++], c, compState);
    } else if (lb.pinnedCount < pinnedTarget &&
               matchesPinned(c["athlete"]["displayName"] | "")) {
      fillRow(lb.pinned[lb.pinnedCount++], c, compState);
    } else if (lb.leaderCount >= LEADER_COUNT &&
               lb.pinnedCount >= pinnedTarget) {
      break;
    }
  }

  out = lb;
  return true;
}
