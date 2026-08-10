#include "display.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Fonts/TomThumb.h>  // 3x5 font from Adafruit GFX: 16 chars per line

static MatrixPanel_I2S_DMA* dma = nullptr;

// ---------------------------------------------------------------------------
// Layout constants (pixel coordinates; text y values are font baselines).
// TomThumb advances 4 px per character, so a 64 px row fits 16 characters.
// ---------------------------------------------------------------------------
static const int HEADER_BASE   = 5;
static const int HEADER_RULE_Y = 7;
static const int LEADER_BASE0  = 14;  // first leader row baseline
static const int ROW_PITCH     = 6;
static const int PINNED_RULE_Y = 42;
static const int PINNED_BASE0  = 48;
static const int POS_X         = 0;
static const int NAME_X        = 13;
static const int SCORE_RIGHT   = 54;  // right edge of the score column
static const int THRU_RIGHT    = 63;  // right edge of the thru column

// Colors (initialised in displayInit, after the driver exists)
static uint16_t C_WHITE, C_GRAY, C_DIM, C_YELLOW, C_RED, C_GREEN, C_CYAN;

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
  if (score[0] == '-') return C_RED;
  if (score[0] == '+') return C_GREEN;
  return C_WHITE;
}

static void drawRow(const GolferRow& row, int baseY, bool pinnedStyle) {
  drawText(POS_X, baseY, row.pos, C_GRAY);

  // Right-hand columns first, so the name knows how much room it has.
  drawTextRight(SCORE_RIGHT, baseY, row.score, scoreColor(row.score));
  drawTextRight(THRU_RIGHT, baseY, row.thru, C_GRAY);

  int scoreStartX = SCORE_RIGHT - textWidth(row.score) + 1;
  int maxChars = (scoreStartX - 2 - NAME_X) / 4;
  char name[sizeof(row.name)];
  strlcpy(name, row.name, sizeof(name));
  if (maxChars >= 0 && maxChars < (int)strlen(name)) name[maxChars] = 0;

  drawText(NAME_X, baseY, name, pinnedStyle ? C_CYAN : C_WHITE);
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

  dma->setBrightness8(PANEL_BRIGHTNESS);
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
  return true;
}

void displayMessage(const char* l1, const char* l2, const char* l3) {
  if (!dma) return;
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

void displayLeaderboard(const Leaderboard& lb, bool fetchOk) {
  if (!dma) return;

  if (!lb.hasEvent) {
    displayMessage("PGA TOUR", "OFF WEEK", "no event");
    return;
  }

  dma->clearScreen();

  // Header: tournament name (truncated to leave room for the round label).
  int labelW = textWidth(lb.roundLabel);
  int nameBudget = (PANEL_WIDTH - 1 - labelW - 3 - 0) / 4;
  char header[sizeof(lb.eventName)];
  strlcpy(header, lb.eventName, sizeof(header));
  if (nameBudget >= 0 && nameBudget < (int)strlen(header)) header[nameBudget] = 0;
  drawText(0, HEADER_BASE, header, C_YELLOW);
  drawTextRight(PANEL_WIDTH - 1, HEADER_BASE, lb.roundLabel, C_WHITE);
  dma->drawFastHLine(0, HEADER_RULE_Y, PANEL_WIDTH, C_DIM);

  for (int i = 0; i < lb.leaderCount; i++) {
    drawRow(lb.leaders[i], LEADER_BASE0 + i * ROW_PITCH, false);
  }

  if (lb.pinnedCount > 0) {
    dma->drawFastHLine(0, PINNED_RULE_Y, PANEL_WIDTH, C_DIM);
    for (int i = 0; i < lb.pinnedCount; i++) {
      drawRow(lb.pinned[i], PINNED_BASE0 + i * ROW_PITCH, true);
    }
  }

  // Status dot, bottom-right: green = fresh data, red = last refresh failed.
  uint16_t dot = fetchOk ? dma->color565(0, 160, 0) : dma->color565(200, 0, 0);
  dma->fillRect(PANEL_WIDTH - 2, PANEL_HEIGHT - 2, 2, 2, dot);
}
