#include "display.h"
#include "settings.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Fonts/TomThumb.h>  // 3x5 font from Adafruit GFX: 16 chars per line
#include <driver/gpio.h>

static MatrixPanel_I2S_DMA* dma = nullptr;

// ---------------------------------------------------------------------------
// Layout constants (pixel coordinates; text y values are font baselines).
// TomThumb advances 4 px per character, so a 64 px row fits 16 characters.
// ---------------------------------------------------------------------------
static const int PAD           = 1;   // 1px margin around all content
static const int HEADER_BASE   = 6;   // baselines shifted +1 vs. edge for 1px top padding
static const int HEADER_RULE_Y = 8;
static const int LEADER_BASE0  = 15;  // first leader row baseline
static const int ROW_PITCH     = 6;   // up to 8 rows fit: baselines 15..57
static const int COL_GAP       = 1;   // 1px between every column
static const int NAME_GAP      = COL_GAP;
static const int TOTAL_RIGHT   = PANEL_WIDTH - 1 - PAD;  // far-right edge: total; also "NEXT UP" status

// Colors (initialised in displayInit, after the driver exists)
static uint16_t C_WHITE, C_GRAY, C_DIM, C_YELLOW, C_RED, C_GREEN, C_CYAN,
    C_ORANGE;

static int textWidth(const char* s) {
  int16_t x1, y1;
  uint16_t w, h;
  dma->getTextBounds(s, 0, 20, &x1, &y1, &w, &h);
  return (int)w;
}

static void drawText(int x, int baseY, const char* s, uint16_t color) {
  dma->setTextColor(color);
  dma->setCursor(x, baseY);
  dma->print(s);
}

static void drawTextRight(int xRight, int baseY, const char* s, uint16_t color) {
  drawText(xRight - textWidth(s) + 1, baseY, s, color);
}

// Golf-TV convention: red for under par. Green for over, white for even.
static uint16_t scoreColor(const char* score) {
  if (strcmp(score, "-") == 0) return C_GRAY;  // no data yet (placeholder dash)
  if (score[0] == '-') return C_RED;    // under par
  if (score[0] == '+') return C_GREEN;  // over par
  return C_WHITE;                        // even
}

// Right-hand columns are each sized to their widest possible value and packed
// against the right edge with a 1px gap; the name flexes into the space left.
// Widths depend only on the (fixed) font, so the edges are computed once.
//   L→R:  NAME (flex) · thru(holes) · today(round score) · total  [far right]
struct ColLayout {
  int thruR, todayR, totalR;  // right edges (right-aligned text)
  int thruLeft;               // reserved left edge of the leftmost column
};

static const ColLayout& columns() {
  static ColLayout L;
  static bool ready = false;
  if (!ready) {
    int totalW = textWidth("-15");  // widest total to par (e.g. "-15", "+10")
    int todayW = textWidth("-15");  // widest single-round to par
    int thruW  = textWidth("18");   // widest holes-played ("17", then "F")
    L.totalR = TOTAL_RIGHT;
    L.todayR = L.totalR - totalW - COL_GAP;
    L.thruR  = L.todayR - todayW - COL_GAP;
    L.thruLeft = L.thruR - thruW + 1;
    ready = true;
  }
  return L;
}

static void drawRow(const GolferRow& row, int baseY) {
  const ColLayout& L = columns();

  // Out of the tournament (cut/withdrawn/DQ): orange badge in the rank column,
  // total still on the right, the per-round columns left blank. The name flexes
  // into the freed space so the row reads "MC  ABERG            -1".
  if (row.out) {
    drawText(PAD, baseY, row.pos, C_ORANGE);
    int nameX = PAD + textWidth(row.pos) + NAME_GAP;
    drawTextRight(L.totalR, baseY, row.score, scoreColor(row.score));
    int maxChars = (L.todayR - nameX + 1) / 4;
    char name[sizeof(row.name)];
    strlcpy(name, row.name, sizeof(name));
    if (maxChars >= 0 && maxChars < (int)strlen(name)) name[maxChars] = 0;
    drawText(nameX, baseY, name, row.selected ? C_CYAN : C_WHITE);
    return;
  }

  // Rank hard against the left margin; the name sits right after it.
  drawText(PAD, baseY, row.pos, C_GRAY);
  int nameX = PAD + textWidth(row.pos) + NAME_GAP;

  // Total (to par) is always the far-right column.
  drawTextRight(L.totalR, baseY, row.score, scoreColor(row.score));

  int nameRight;  // last x the name may use
  if (row.tee[0]) {
    // Yet to tee off: the tee time (gray, so it reads as status) fills the
    // holes + round-score columns; the total score stays on the right.
    drawTextRight(L.todayR, baseY, row.tee, C_GRAY);
    nameRight = (L.todayR - textWidth(row.tee) + 1) - COL_GAP;
  } else {
    // Playing / finished: holes played (left), then this round's score. Name
    // clips at the reserved column edge so rows stay vertically aligned.
    drawTextRight(L.todayR, baseY, row.today, scoreColor(row.today));
    drawTextRight(L.thruR,  baseY, row.thru,  C_GRAY);
    nameRight = L.thruLeft - COL_GAP;
  }

  int maxChars = (nameRight - nameX + 1) / 4;
  char name[sizeof(row.name)];
  strlcpy(name, row.name, sizeof(name));
  if (maxChars >= 0 && maxChars < (int)strlen(name)) name[maxChars] = 0;

  drawText(nameX, baseY, name, row.selected ? C_CYAN : C_WHITE);
}

// ---------------------------------------------------------------------------
// Loading animation: a golf ball rolls along the green toward the flag.
// Runs on its own task so it survives the main task blocking on TLS reads.
// The task only touches the ball band (x < 48, three rows above the ground)
// with stateless fillRect calls — text (shared GFX cursor) stays on the
// main task, so the two never contend.
// ---------------------------------------------------------------------------

static const int GROUND_Y   = 44;  // the green
static const int BALL_MIN_X = 2;
static const int BALL_MAX_X = 45;  // stops at the lip of the hole
static const int HOLE_X     = 47;
static const int FLAG_X     = 50;

static volatile bool animRun = false;
static TaskHandle_t animTask = nullptr;
static char sLoadingStatus[16] = "";  // current status word, e.g. "FETCHING"

// Redraws the status line with `dots` trailing dots (0-3). The left edge is
// anchored on the widest form ("WORD...") so the word doesn't jitter sideways
// as the dots grow. Runs only on the animation task (see below), so it never
// contends with the main task for the shared GFX text cursor.
static void drawLoadingStatus(int dots) {
  char full[20];
  snprintf(full, sizeof(full), "%s...", sLoadingStatus);
  int x = (PANEL_WIDTH - textWidth(full)) / 2;
  if (x < 0) x = 0;
  char shown[20];
  snprintf(shown, sizeof(shown), "%s%.*s", sLoadingStatus, dots, "...");
  dma->fillRect(0, 54, PANEL_WIDTH, 10, 0);
  drawText(x, 61, shown, C_GRAY);
}

static void loadingAnimTask(void*) {
  int x = BALL_MIN_X;
  int holdFrames = 0;           // brief pause after the ball drops in
  int dotFrame = 0, dots = 0;   // status dots cycle 0->1->2->3 on their own beat
  drawLoadingStatus(dots);
  while (animRun) {
    dma->fillRect(0, GROUND_Y - 3, 48, 3, 0);  // erase ball band only
    if (holdFrames > 0) {
      holdFrames--;                              // ball in the hole: hold
    } else if (x <= BALL_MAX_X) {
      dma->fillRect(x, GROUND_Y - 2, 2, 2, C_WHITE);
      x++;
    } else {
      x = BALL_MIN_X;                            // tee up again after a beat
      holdFrames = 11;                           // ~500ms at 45ms/frame
    }
    // Advance the dots ~every 400ms, independent of the ball, so "FETCHING..."
    // keeps animating even while the ball is paused in the hole.
    if (++dotFrame >= 9) {
      dotFrame = 0;
      dots = (dots + 1) % 4;
      drawLoadingStatus(dots);
    }
    vTaskDelay(pdMS_TO_TICKS(45));
  }
  animTask = nullptr;
  vTaskDelete(nullptr);
}

static void drawLoadingScene() {
  dma->clearScreen();
  drawText((PANEL_WIDTH - textWidth("GOLF")) / 2, 12, "GOLF", C_YELLOW);
  drawText((PANEL_WIDTH - textWidth("LEADERBOARD")) / 2, 19, "LEADERBOARD",
           C_YELLOW);

  uint16_t green = dma->color565(0, 120, 40);
  dma->drawFastHLine(0, GROUND_Y, PANEL_WIDTH, green);
  dma->fillRect(HOLE_X, GROUND_Y, 3, 2, 0);                    // the hole
  dma->drawFastVLine(FLAG_X, GROUND_Y - 14, 14, C_WHITE);      // pin
  for (int r = 0; r < 4; r++) {                                // flag
    dma->drawFastHLine(FLAG_X - 4 + r, GROUND_Y - 14 + r, 4 - r, C_RED);
  }
}

void displayLoading(const char* status) {
  if (!dma) return;
  // Publish the status word; the animation task owns the actual text drawing
  // (and its dot animation), so main and anim never share the GFX cursor.
  strlcpy(sLoadingStatus, status, sizeof(sLoadingStatus));
  if (!animRun) {
    drawLoadingScene();
    animRun = true;
    xTaskCreate(loadingAnimTask, "loadanim", 4096, nullptr, 1, &animTask);
  }
}

void displayLoadingStop() {
  if (!animRun) return;
  animRun = false;
  while (animTask != nullptr) delay(1);  // wait for the task's last frame
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool displayInit() {
  HUB75_I2S_CFG::i2s_pins pins = {
      HUB75_R1, HUB75_G1, HUB75_B1, HUB75_R2, HUB75_G2, HUB75_B2,
      HUB75_A,  HUB75_B,  HUB75_C,  HUB75_D,  HUB75_E,
      HUB75_LAT, HUB75_OE, HUB75_CLK,
  };
  HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN, pins);
  // If your panel stays black or shows garbage, it likely uses FM6126A
  // driver chips — uncomment the next line.
  // cfg.driver = HUB75_I2S_CFG::FM6126A;

  dma = new MatrixPanel_I2S_DMA(cfg);
  if (!dma->begin()) return false;

  dma->setBrightness8(settings.brightness);
  dma->setFont(&TomThumb);
  dma->setTextWrap(false);
  dma->clearScreen();

  C_WHITE  = dma->color565(255, 255, 255);
  C_GRAY   = dma->color565(140, 140, 140);
  C_DIM    = dma->color565(55, 55, 55);
  C_YELLOW = dma->color565(255, 200, 0);
  C_RED    = dma->color565(255, 60, 60);
  C_GREEN  = dma->color565(0, 210, 90);
  C_CYAN   = dma->color565(0, 210, 255);
  C_ORANGE = dma->color565(255, 130, 0);
  return true;
}

void displaySetBrightness(uint8_t b) {
  if (dma) dma->setBrightness8(b);
}

void displayMessage(const char* l1, const char* l2, const char* l3) {
  if (!dma) return;
  displayLoadingStop();
  dma->clearScreen();
  const char* lines[3] = {l1, l2, l3};
  const uint16_t colors[3] = {C_YELLOW, C_WHITE, C_GRAY};
  int y = 24;
  for (int i = 0; i < 3; i++) {
    if (!lines[i]) continue;
    drawText((PANEL_WIDTH - textWidth(lines[i])) / 2, y, lines[i], colors[i]);
    y += 10;
  }
}

// Greedy word-wrap of `src` into two lines of up to 16 chars each; anything
// that doesn't fit is dropped (the panel is only so big).
static void wrapTwoLines(const char* src, char* l1, char* l2) {
  const size_t MAXC = 16;
  l1[0] = l2[0] = 0;
  char* lines[2] = {l1, l2};
  int line = 0;
  const char* w = src;
  while (*w && line < 2) {
    while (*w == ' ') w++;
    const char* end = strchr(w, ' ');
    size_t wordLen = end ? (size_t)(end - w) : strlen(w);
    size_t curLen = strlen(lines[line]);
    size_t need = wordLen + (curLen ? 1 : 0);
    if (curLen + need > MAXC) {
      if (curLen == 0) {  // single word longer than a line: hard cut
        strncat(lines[line], w, MAXC);
        w += wordLen;
      }
      line++;
      continue;
    }
    if (curLen) strcat(lines[line], " ");
    strncat(lines[line], w, wordLen);
    w += wordLen;
  }
}

// MODE_NEXT: what's coming up, and whether the pinned golfers are playing.
static void renderNext(const Leaderboard& lb) {
  drawText(PAD, HEADER_BASE, "NEXT UP", C_YELLOW);
  dma->drawFastHLine(PAD, HEADER_RULE_Y, PANEL_WIDTH - 2 * PAD, C_DIM);

  char l1[17], l2[17];
  wrapTwoLines(lb.nextName, l1, l2);
  drawText(PAD, 16, l1, C_WHITE);
  if (l2[0]) drawText(PAD, 22, l2, C_WHITE);
  drawText(PAD, 30, lb.nextDates, C_GRAY);

  if (lb.nextGolferCount > 0) {
    dma->drawFastHLine(PAD, 35, PANEL_WIDTH - 2 * PAD, C_DIM);
    for (int i = 0; i < lb.nextGolferCount; i++) {
      const NextGolfer& g = lb.nextGolfers[i];
      int baseY = 43 + i * ROW_PITCH;
      drawText(PAD, baseY, g.name, C_CYAN);
      uint16_t c = C_GRAY;                        // TBD: field not out yet
      if (strcmp(g.status, "IN") == 0) c = C_GREEN;
      else if (strcmp(g.status, "OUT") == 0) c = C_ORANGE;
      drawTextRight(TOTAL_RIGHT, baseY, g.status, c);
    }
  }
}

// MODE_LIVE: the leaderboard itself.
static void renderLive(const Leaderboard& lb) {

  // Header: tournament name (truncated to leave room for the round label).
  int labelW = textWidth(lb.roundLabel);
  int nameBudget = (PANEL_WIDTH - 2 * PAD - labelW - 3) / 4;
  char header[sizeof(lb.eventName)];
  strlcpy(header, lb.eventName, sizeof(header));
  if (nameBudget >= 0 && nameBudget < (int)strlen(header)) header[nameBudget] = 0;
  drawText(PAD, HEADER_BASE, header, C_YELLOW);
  drawTextRight(PANEL_WIDTH - 1 - PAD, HEADER_BASE, lb.roundLabel, C_WHITE);
  dma->drawFastHLine(PAD, HEADER_RULE_Y, PANEL_WIDTH - 2 * PAD, C_DIM);

  for (int i = 0; i < lb.leaderCount; i++) {
    drawRow(lb.leaders[i], LEADER_BASE0 + i * ROW_PITCH);
  }

  if (lb.pinnedCount > 0) {
    // Divider sits 2px below the last leader; the pinned block follows with the
    // same breathing room as the header rule. The leader count is dynamic, so
    // these are derived from it rather than fixed (6 leaders -> rule at 47).
    int lastLeaderBase = LEADER_BASE0 + (lb.leaderCount - 1) * ROW_PITCH;
    int pinnedRuleY = lastLeaderBase + 2;
    int pinnedBase0 = pinnedRuleY + 7;
    dma->drawFastHLine(PAD, pinnedRuleY, PANEL_WIDTH - 2 * PAD, C_DIM);
    for (int i = 0; i < lb.pinnedCount; i++) {
      drawRow(lb.pinned[i], pinnedBase0 + i * ROW_PITCH);
    }
  }
}

void displayLeaderboard(const Leaderboard& lb, bool fetchOk) {
  if (!dma) return;
  displayLoadingStop();
  dma->clearScreen();
  switch (lb.mode) {
    case MODE_LIVE: renderLive(lb); break;
    case MODE_NEXT: renderNext(lb); break;
    case MODE_NONE:
      drawText((PANEL_WIDTH - textWidth("PGA TOUR")) / 2, 26, "PGA TOUR", C_YELLOW);
      drawText((PANEL_WIDTH - textWidth("SEASON OVER")) / 2, 36, "SEASON OVER", C_WHITE);
      break;
  }
  (void)fetchOk;  // staleness indicator (the corner dot) removed for space
}

void displayPowerOff() {
  displayLoadingStop();
  if (dma) {
    dma->clearScreen();
    dma->stopDMAoutput();
    delay(10);
  }
  // With DMA stopped (or never started) the HUB75 inputs float. OE is
  // active-low: park it high so every row stays disabled, and hold the
  // level through deep sleep.
  pinMode(HUB75_OE, OUTPUT);
  digitalWrite(HUB75_OE, HIGH);
  gpio_hold_en((gpio_num_t)HUB75_OE);
  gpio_deep_sleep_hold_en();
}

void displayReleaseHolds() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)HUB75_OE);
}
