# Power plan — Golf leaderboard

How this project is powered, written for a fresh reader. This version
incorporates an expert review (2026-08): the panel is fed **directly** from the
supply, and the ESP32 only taps that rail — so no panel current flows through
the ESP32 board.

## What the project is

A **64×64 P2 HUB75 RGB LED matrix panel** (a "screen" of 4096 LEDs) driven by a
small **Waveshare ESP32-S3-Zero** microcontroller. The ESP32 renders a golf
leaderboard onto the panel. Everything runs at **5 volts DC**.

## The parts involved in power

| Part | Role | Power notes |
|---|---|---|
| Waveshare ESP32-S3-Zero | Drives the panel's data lines | Has a **`5V`** pad and a **`GND`** pad. During deployment the `5V` pad is a power **input**. |
| 64×64 HUB75 panel | The display | Needs **5 V**. Power cable ends in red(+)/black(−) fork terminals. |
| 5 V / 2 A USB-A charger | The power source | Standard wall charger, 5 V / 2 A. |
| DC-terminal adapter (came with panel) | Screw-terminal junction (barrel jack + `+`/`−` screws) | The shared 5 V node. |

## How power flows (reviewed design)

The panel is powered **directly** from the charger at the screw terminal. The
ESP32 taps the same 5 V rail. **No panel current passes through the ESP32.**

```
[5V/2A charger] ──> [DC-adapter screw terminals +/−]
                          │  (panel current stops here)
      ┌───────────────────┼───────────────────────┐
      ▼                   ▼                        ▼
 panel +5V (red fork)  panel GND (black fork)   branch to ESP32:
                                                5V pad (red) + GND pad (black)
```

Getting the charger's 5 V into the terminal — either:
- **USB-A → 5.5×2.1 mm barrel-jack cable** into the adapter's barrel socket
  (no cutting), or
- **cut a spare USB-A cable**, strip the **red (+5 V)** and **black (GND)** wires
  into the `+`/`−` screws (cut away the green/white data wires).
  **Verify red = +5 V vs black with a multimeter first** — cable colors vary.

Polarity is consistent end-to-end: red = +5 V, black = GND.

### Pads NOT to use for power
- `3V3` (third pad below `GND`) is a 3.3 V **output**, never a power input.
- Only `5V` (top pad, next to USB-C) and `GND` (second pad) are used.

## Why direct-feed, not through the ESP32 (review outcome)

The earlier draft powered the panel *through* the ESP32's `5V` pin. Review
flagged two problems with routing ~1 A that way on this tiny board:
1. **Thin PCB traces** between the USB-C connector and the `5V` pad — risk of
   voltage drop and heating.
2. **A likely Schottky protection diode** (USB VBUS → `5V` pin) rated ~500 mA–
   1 A; sustained panel current could burn it and disable USB power.

Feeding the panel directly removes both risks: the ESP32 only ever draws its own
~0.3 A, and the panel's current never touches the board.

## Current budget (confirmed adequate)

| Load | Typical | Peak |
|---|---|---|
| Panel (brightness 90/255, mostly-dark leaderboard) | ~0.2–0.5 A | ~1 A |
| ESP32-S3 incl. Wi-Fi bursts | ~0.15 A | ~0.5 A |
| **Whole system** | **~0.5–1 A** | **~1.5 A** |

A 64×64 panel draws ~4 A only at full-white/100 % brightness, which the firmware
never renders. The 5 V / 2 A charger has comfortable margin.

## Programming / flashing procedure (IMPORTANT — protects the laptop)

During flashing the ESP32 connects to a computer's USB-C (for programming +
serial logs). Rule: **never tie the computer's 5 V and the charger's 5 V
together.**

- **Deployment (no computer):** charger → terminal → panel **and** the `5V`
  branch to the ESP32. One 5 V source.
- **Programming (computer attached):** ESP32 powered by the **computer's USB-C**;
  **disconnect the `5V` branch wire** (ESP32 `5V` ↔ terminal). Panel still
  powered by the charger via the terminal. **Keep only the GND wire connected.**

So: the single `5V` branch jumper is connected **only when the computer is
unplugged**, and vice versa. The **GND** wire stays connected always (shared
ground is required for the data signals; tying grounds is safe — tying the two
5 V rails is what you avoid).

## Safety rules

1. **5 V only** — a 9 V/12 V supply would destroy the panel.
2. **Polarity:** red → +5 V, black → GND, never reversed.
3. **Wire with power disconnected, energize last.**
4. **Shared ground** between panel and ESP32 (needed for data).
5. **One 5 V source to the ESP32 at a time** (see programming procedure).

## Out of scope but noted: 3.3 V vs 5 V data logic

The ESP32 drives the HUB75 data lines at 3.3 V; the panel expects 5 V logic. It
usually works as-is. **If the display later flickers or shows wrong colors, add
a level shifter (e.g. 74HCT245)** — a data-signal fix, not a power fix.

## Note: data wiring is separate

14 signal wires carry display data from the ESP32's GPIO pins to the panel.
They carry negligible current and are outside this power analysis, except that
the shared **GND** also serves as their reference.
