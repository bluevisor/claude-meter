# Clawdmeter

A tiny ESP32 dashboard for keeping a glanceable eye on Claude Code from
across the desk. It pairs with your laptop over Bluetooth LE and mirrors
the same data your statusline would — current model, context usage, 5h
and 7d quotas — and flips into a "working" view with a pixel-art Clawd
that thinks, has ideas, and watches the tokens roll in while a turn is
running.

## Hardware

Two boards are supported (selectable via PlatformIO env):

- **Waveshare ESP32-S3-LCD-1.3** *(default — small, no touch, no
  buttons, runs off USB)* — 240×240 ST7789V2 SPI panel, QMI8658 IMU.
- **Waveshare ESP32-S3-Touch-AMOLED-2.16** *(legacy upstream target)* —
  480×480 CO5300 AMOLED, CST9220 touch, AXP2101 PMU, Li-Po battery.

The LCD-1.3 build is what's described from here on.

## Screens

There are two main screens; the firmware auto-switches between them
based on whether Claude Code is mid-turn.

### Usage screen (idle)

Shown whenever Claude Code is idle (JSONL has been quiet for ≥60s).

- **Title**: the current model, e.g. `Opus 4.7 1M`, derived from
  `~/.claude/settings.json` + the transcript's `model` field.
- **MAX / API** badge next to the tiny Clawd sprite on the right.
- **Current** row: 5h utilization % + reset time.
- **Weekly** row: 7d utilization % + reset time.

### Working screen (active)

Shown whenever the JSONL was touched in the last 60 seconds — i.e.
Claude Code is actively running a turn.

- A big pixel-art Clawd bobs / sways at the top; eye expressions cycle
  through blink, squint, look-left / right, wide / surprise, sleepy.
- **Tokens + elapsed time** for the current task ("current task" =
  since the most recent human user message; tool-result echoes are
  excluded). The timer ticks 1 second per second locally, only
  re-syncing to the host every ~15s.
- **CONTEXT bar** showing current context tokens vs the model's
  ceiling, with a health gradient:
  green → dark-green → blue → yellow → orange → red as the window
  fills up.

#### Thinking / lightbulb beats

While inside a working turn the firmware also reacts to which content
block the model is producing:

- **Thinking** — the body holds still (no bob/sway) and runs a
  slower, contemplative expression cycle with a small thought-dot
  beside the head. Triggered when the latest assistant message is a
  `thinking` block written < 6s ago.
- **Lightbulb** — Clawd is replaced by a yellow pixel-art lightbulb
  that twinkles for a few beats. Triggered when a `text` or
  `tool_use` block follows a `thinking` block.

## How it works

The daemon reads from the same source Claude Code's statusline reads:
the active session transcript at
`~/.claude/projects/<project>/<session>.jsonl`. The most-recently-touched
JSONL wins. From each assistant message's `usage` block it pulls token
counts, content-block type (for the thinking / lightbulb phase), and the
configured model. It also calls `api.anthropic.com/v1/messages` for the
5h / 7d quota headers (one-token Haiku ping, basically free) when there
is no active session, or alongside the session data when there is one.

The payload is written to a custom GATT RX characteristic on the ESP32;
the firmware parses it and updates the LVGL UI. The on-device working
timer is anchored locally so the daemon doesn't have to broadcast every
second.

## BLE protocol

The device advertises with appearance `0x0140` (Generic Display) — *not*
HID, so macOS / Linux won't try to bind it as a keyboard. Bonding is
disabled to make "Forget This Device" a clean operation; the data is
just usage %, not sensitive.

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| REQ (notify)               | `4c41555a-4465-7669-6365-000000000004` |

JSON payload (written to RX):

```json
{
  "mode": "subscription",
  "s": 33, "sr": 120,
  "w": 10, "wr": 3670,
  "cu": 187346, "cm": 1000000,
  "act": 1,
  "tt": 25778, "ts": 306,
  "ml": "Opus 4.7 1M",
  "ph": "thinking",
  "st": "allowed", "ok": true
}
```

| Field   | Meaning                                                        |
| ------- | -------------------------------------------------------------- |
| `mode`  | `subscription` (rate-limit headers) or `api` (LiteLLM proxy)   |
| `s/sr`  | 5h utilization %, minutes until reset                          |
| `w/wr`  | 7d utilization %, minutes until reset                          |
| `cu/cm` | Current context tokens, context-window ceiling                 |
| `act`   | 1 = Claude Code mid-turn → working screen; 0 = idle → usage    |
| `tt/ts` | Output tokens for the current task, seconds elapsed            |
| `ml`    | Model display label, e.g. `Opus 4.7 1M`                        |
| `ph`    | `working` / `thinking` / `lightbulb`                           |

## Installation

### Prerequisites

- Linux or macOS
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Claude Code (the daemon reads its session transcripts)

### Flash the firmware

```bash
pio run -d firmware -t upload                            # auto-detect port
pio run -d firmware -t upload --upload-port /dev/tty.usbmodemXXXX   # explicit
```

### macOS daemon

```bash
./install-mac.sh
```

This creates a Python venv in `daemon/.venv/`, installs `bleak` +
`httpx`, drops a LaunchAgent at
`~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and starts
it. The first run is launched interactively so macOS prompts for
Bluetooth permission.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # is it running?
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

### Linux daemon

```bash
./install.sh
systemctl --user start claude-usage-daemon
journalctl --user -u claude-usage-daemon -f                                 # logs
```

### Pairing

The meter advertises as `Claude Meter` with appearance Generic Display.
On macOS you don't need to pair it — the daemon connects via
CoreBluetooth directly with the cached UUID at
`~/.config/claude-usage-monitor/ble-address`. On Linux, pair once with
`bluetoothctl` (`pair <MAC>`, then `trust <MAC>`).

### 1M-context override

If the daemon can't infer the 1M variant from `~/.claude/settings.json`
(for example you switched off `[1m]` in settings.json but the live
sessions are still running on it), set an explicit ceiling:

```bash
CLAUDE_METER_CONTEXT_MAX=1000000 launchctl load -w \
    ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
```

## Repository layout

```
firmware/        PlatformIO project — ESP32 firmware (LVGL 9 + NimBLE)
  src/
    main.cpp           setup/loop, LVGL flush, payload parser
    ui.cpp             three screens, phase-driven animation
    clawd_sprite.cpp   16×10 pixel-art frames (idle, expressions,
                       lightbulb)
    ble.cpp            custom GATT service, no HID
    theme.h            palette
daemon/
  claude_usage_daemon.py     macOS / Linux Python daemon (bleak)
  claude-usage-daemon.sh     legacy Linux shell daemon (bluetoothctl)
install-mac.sh / install.sh  one-shot bootstrappers
```

## Credits

- Pixel-art Clawd designs were originally inspired by
  [@amaanbuilds](https://x.com/amaanbuilds)'s
  [claudepix.vercel.app](https://claudepix.vercel.app) library; the
  current build embeds a hand-traced sprite plus an animated lightbulb.
- The macOS daemon port + LaunchAgent harness were originally by
  [Chris Davidson](https://github.com/lorddavidson) — thanks Chris!

## Licensing note

This repo uses Anthropic-branded fonts and a stylised Clawd mascot. The
code I've written here is non-proprietary, but the embedded assets are
copyrighted by Anthropic. I'm not licensing the repo under a copyleft
license because of that — if you fork or copy, be aware of the asset
provenance.
