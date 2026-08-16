# Golf Live Update — agent notes

## Hard rules

- **NEVER read, edit, overwrite, or print the contents of `include/secrets.h`.**
  It holds the user's real Wi-Fi credentials, is gitignored, and is maintained
  by hand. If a build needs it, assume it already exists. To change what fields
  it should contain, edit `include/secrets.h.example` instead and tell the user
  to update their local copy.

## Project shape

64×64 P2 HUB75 RGB matrix driven by an ESP32-S3 (Waveshare S3-Zero / Electrokit
"S3 mini"). Pulls the PGA leaderboard from ESPN's keyless scoreboard endpoint,
renders top-5 + pinned golfers. Night-time deep-sleep schedule. PlatformIO build.

- `pio run -e s3mini -t upload -t monitor` — flash the real board + watch logs
- `pio run -e wokwi` — browser simulation, no hardware (see README)
- Pin map, brightness, pinned golfers, night hours: `include/config.h`
