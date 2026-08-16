#include "settings.h"

#include <Preferences.h>

#include "config.h"

Settings settings;

static Preferences prefs;
static const char* NS = "golf";

void settingsLoad() {
  prefs.begin(NS, /*readOnly=*/true);

  settings.brightness   = prefs.getUChar("bright", PANEL_BRIGHTNESS);
  settings.nightEnabled = prefs.getBool("nEn", NIGHT_MODE_ENABLED);
  settings.nightStart   = prefs.getUChar("nStart", NIGHT_START_HOUR);
  settings.nightEnd     = prefs.getUChar("nEnd", NIGHT_END_HOUR);

  int n = prefs.getInt("pinN", -1);
  if (n < 0) {
    // First boot: seed the tracked list from config.h.
    settings.pinnedCount = min((size_t)MAX_PINNED_GOLFERS, PINNED_GOLFER_COUNT);
    for (uint8_t i = 0; i < settings.pinnedCount; i++)
      strlcpy(settings.pinned[i], PINNED_GOLFERS[i], PINNED_NAME_MAX);
  } else {
    settings.pinnedCount = min(n, (int)MAX_PINNED_GOLFERS);
    for (uint8_t i = 0; i < settings.pinnedCount; i++) {
      char key[8];
      snprintf(key, sizeof(key), "pin%u", i);
      strlcpy(settings.pinned[i], prefs.getString(key, "").c_str(), PINNED_NAME_MAX);
    }
  }

  prefs.end();
}

void settingsSave() {
  prefs.begin(NS, /*readOnly=*/false);

  prefs.putUChar("bright", settings.brightness);
  prefs.putBool("nEn", settings.nightEnabled);
  prefs.putUChar("nStart", settings.nightStart);
  prefs.putUChar("nEnd", settings.nightEnd);
  prefs.putInt("pinN", settings.pinnedCount);
  for (uint8_t i = 0; i < settings.pinnedCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "pin%u", i);
    prefs.putString(key, settings.pinned[i]);
  }

  prefs.end();
}
