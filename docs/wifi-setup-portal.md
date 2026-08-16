# Wi-Fi setup portal (WiFiManager) — portable notes

A reusable pattern for configuring Wi-Fi on an ESP32/ESP8266 from a phone,
instead of hardcoding credentials. Copy into any project.

## Concept

On first boot the device configures itself: it starts its own Wi-Fi access
point, you join it with a phone, a web page ("captive portal") lets you pick
your network and enter the password, and the device saves those credentials to
its **flash (NVS)** — so it reconnects on its own afterward, no reflashing.

## Flow (`tzapu/WiFiManager` library)

1. On boot, `wm.autoConnect("SetupAP-name")` first tries the **saved**
   credentials from flash.
2. If none exist or they fail, it becomes an **access point** (`SetupAP-name`)
   and runs a captive portal (a DNS trick makes the config page auto-open on the
   phone).
3. You submit your network + password; it saves them and reconnects as a normal
   client. `autoConnect` **blocks** until connected.
4. Next boot, step 1 succeeds instantly — the portal never appears unless forced.

## Minimal code

```cpp
#include <WiFiManager.h>

void connectWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(0);              // keep portal open until configured
  wm.setAPCallback([](WiFiManager*){         // optional: show setup info (e.g. on a display)
    Serial.printf("Join AP, open http://%s\n", WiFi.softAPIP().toString().c_str());
  });

  bool ok = wm.autoConnect("SetupAP-name");  // tries saved creds, else opens portal
  if (!ok) { ESP.restart(); }                // gave up — reboot and retry
}
```

## Re-configure later (optional button trigger)

Tap a button shortly *after* boot and call `startConfigPortal` instead — it
forces the portal even when credentials are already saved:

```cpp
if (buttonTappedWithinWindow()) wm.startConfigPortal("SetupAP-name");
else                            wm.autoConnect("SetupAP-name");
```

Read the button **after** boot, not held during reset — GPIO0/BOOT held at
reset drops the ESP32 into its USB bootloader instead of running the app.

## Handy extras

- Erase saved credentials: `wm.resetSettings();`
- Silence library logs: `wm.setDebugOutput(false);`

## PlatformIO

```ini
lib_deps =
    tzapu/WiFiManager@^2.0.17
```
Adds ~110 KB flash. Works on ESP32 and ESP8266.

## Why it's worth it

Credentials live in the device's NVS, not the firmware — so one compiled binary
works on any network, and the device can move networks without a rebuild. Two
calls do the real work: `autoConnect()` (try-saved-else-portal) and
`startConfigPortal()` (force-portal); the AP, captive-portal redirect, HTML
form, storage, and auto-reconnect are all handled inside the library.
