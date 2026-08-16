#include "webconfig.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "display.h"
#include "settings.h"

static WebServer server(80);
static const Leaderboard* g_board = nullptr;
static volatile bool* g_refresh = nullptr;

static String esc(const char* s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default:  o += *s;
    }
  }
  return o;
}

static String statusHtml() {
  String s = "<div class=st>";
  if (g_board && g_board->mode == MODE_LIVE)
    s += "<b>" + esc(g_board->eventName) + "</b> " + esc(g_board->roundLabel);
  else if (g_board && g_board->mode == MODE_NEXT)
    s += "Next: <b>" + esc(g_board->nextName) + "</b>";
  else
    s += "No live event";
  s += "<br>IP " + WiFi.localIP().toString();
  s += " &middot; " + String(WiFi.RSSI()) + " dBm";
  s += " &middot; up " + String(millis() / 60000) + " min";
  s += "</div>";
  return s;
}

static void handleRoot() {
  String h = F(
      "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Golf Board</title><style>"
      "body{font-family:system-ui,sans-serif;max-width:430px;margin:14px auto;padding:0 12px;color:#111}"
      "h1{font-size:20px}label{display:block;margin:10px 0 3px;font-weight:600;font-size:14px}"
      "input[type=text],input[type=number]{width:100%;padding:7px;box-sizing:border-box;font-size:15px}"
      "input[type=range]{width:100%}.row{display:flex;gap:8px}.row>div{flex:1}"
      "button{margin-top:14px;padding:10px;font-size:15px;border:0;border-radius:6px;background:#116;color:#fff;width:100%}"
      ".st{background:#f1f1f5;border-radius:6px;padding:10px;font-size:13px;margin:8px 0}"
      ".sec{border-top:1px solid #ddd;margin-top:16px}"
      "button.alt{background:#555}button.warn{background:#a30}"
      "</style><h1>&#9971; Golf Board</h1>");
  h += statusHtml();

  h += F("<form method=post action=/save><label>Brightness</label>");
  h += "<input type=range name=bright min=0 max=255 value=" + String(settings.brightness) + ">";

  h += F("<div class=sec></div><label><input type=checkbox name=nEn ");
  if (settings.nightEnabled) h += "checked";
  h += F("> Night mode (panel sleeps)</label><div class=row><div><label>From (h)</label>");
  h += "<input type=number name=nStart min=0 max=23 value=" + String(settings.nightStart) + "></div>";
  h += F("<div><label>To (h)</label>");
  h += "<input type=number name=nEnd min=0 max=23 value=" + String(settings.nightEnd) + "></div></div>";

  h += F("<div class=sec></div><label>Tracked golfers (surname)</label>");
  for (uint8_t i = 0; i < MAX_PINNED_GOLFERS; i++) {
    String v = i < settings.pinnedCount ? esc(settings.pinned[i]) : String();
    h += "<input type=text name=pin" + String(i) + " value=\"" + v +
         "\" placeholder=\"e.g. Aberg\">";
  }

  h += F(
      "<button type=submit>Save</button></form>"
      "<div class=sec></div>"
      "<form method=post action=/refresh><button class=alt>Refresh now</button></form>"
      "<form method=post action=/restart><button class=alt>Restart</button></form>"
      "<form method=post action=/wifi onsubmit=\"return confirm('Reconfigure Wi-Fi? "
      "The board will open its GolfBoard-setup hotspot.')\">"
      "<button class=warn>Reconfigure Wi-Fi</button></form>");
  server.send(200, "text/html", h);
}

static void handleSave() {
  if (server.hasArg("bright"))
    settings.brightness = constrain(server.arg("bright").toInt(), 0, 255);
  settings.nightEnabled = server.hasArg("nEn");
  if (server.hasArg("nStart"))
    settings.nightStart = constrain(server.arg("nStart").toInt(), 0, 23);
  if (server.hasArg("nEnd"))
    settings.nightEnd = constrain(server.arg("nEnd").toInt(), 0, 23);

  uint8_t pc = 0;
  for (uint8_t i = 0; i < MAX_PINNED_GOLFERS; i++) {
    String v = server.arg(("pin" + String(i)).c_str());
    v.trim();
    if (v.length()) {
      strlcpy(settings.pinned[pc], v.c_str(), PINNED_NAME_MAX);
      pc++;
    }
  }
  settings.pinnedCount = pc;

  settingsSave();
  displaySetBrightness(settings.brightness);

  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleRefresh() {
  if (g_refresh) *g_refresh = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

static void handleRestart() {
  server.send(200, "text/html",
              "<meta http-equiv=refresh content='5;url=/'>Restarting&hellip;");
  delay(300);
  ESP.restart();
}

static void handleWifi() {
  server.send(200, "text/html",
              "Join the <b>GolfBoard-setup</b> Wi-Fi to reconfigure. Restarting&hellip;");
  delay(400);
  WiFi.disconnect(true, true);  // erase stored credentials
  delay(200);
  ESP.restart();
}

void webconfigBegin(const Leaderboard* board, volatile bool* refreshFlag) {
  g_board = board;
  g_refresh = refreshFlag;

  if (MDNS.begin("golfboard")) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_POST, handleRefresh);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.begin();

  Serial.printf("[web] settings at http://golfboard.local/  (http://%s/)\n",
                WiFi.localIP().toString().c_str());
}

void webconfigLoop() { server.handleClient(); }
