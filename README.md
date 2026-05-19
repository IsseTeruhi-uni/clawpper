# Clawpper

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)


A physical approval system for [Claude Code](https://claude.ai/code) using a Flipper Zero. Before Claude Code runs a Bash command, it asks for confirmation on your Flipper Zero — and notifies you when the task is complete.

```
Claude Code ──(PreToolUse hook)──► flipper_bridge.py ──(USB serial)──► Flipper Zero
                                                                              │
                                                                   Show confirm dialog
                                                                   User selects option
                                                                              │
Claude Code ◄──(exit 0 = allow / exit 2 = block)──────────────────────────────
```

## Features

- **Confirm before execution** — Flipper shows a scrollable menu of options (Yes / Yes, allow all / No) before any Bash command runs
- **Task complete notification** — Flipper vibrates and shows "Complete!" when Claude Code finishes a task
- **Session-wide allow** — Select "Yes, allow all bash (session)" to skip future confirmations for the rest of the session
- **Animated Clawd character** — The Flipper screen shows Clawd (Claude's mascot) working at a PC, with state-based animations (waiting / complete / confirm)
- **Dynamic options** — The options shown on Flipper match the tool being used (Bash / Edit / WebFetch)

## Requirements

- Flipper Zero (connected via USB)
- macOS (the bridge script uses macOS serial paths; Linux paths are similar)
- [ufbt](https://github.com/flipperdevices/flipperzero-ufbt) — Flipper build tool
- Python 3
- Claude Code CLI

## Project Structure

```
clawpper/
├── flipper-app/
│   ├── claude_notify.c     # Flipper app (animation, CLI handlers)
│   └── application.fam     # App metadata (appid: clawpper)
├── bridge/
│   └── flipper_bridge.py   # macOS bridge script (run by Claude Code hooks)
├── .claude/
│   └── settings.json       # Claude Code hook configuration
└── install.sh              # One-step installer
```

## Setup

### 1. Install ufbt

```bash
pip3 install ufbt
ufbt update
```

### 2. Build and install the Flipper app

Connect your Flipper Zero via USB, then:

```bash
cd flipper-app
ufbt launch
```

This builds `clawpper.fap` and installs it directly to `/ext/apps/Tools/` on the Flipper.

To install manually via [qFlipper](https://flipperzero.one/update), run `ufbt build` and copy `dist/clawpper.fap` instead.

### 3. Run the installer

```bash
./install.sh
```

This does two things:
- Copies `bridge/flipper_bridge.py` to `~/.claude/flipper_bridge.py`
- Merges the hook configuration into `~/.claude/settings.json`

### 4. Verify

Launch the Clawpper app on your Flipper Zero (Apps → Tools → Clawpper), then test from your terminal:

```bash
# Trigger a confirmation dialog on Flipper
echo "hello"
```

Claude Code should now pause before running any Bash command and wait for your approval on the Flipper.

## How It Works

### Hooks

`~/.claude/settings.json` registers two hooks with Claude Code:

| Hook | Trigger | Action |
|------|---------|--------|
| `PreToolUse` (Bash) | Before any Bash command | Sends confirm request to Flipper, blocks until user responds |
| `Stop` | When Claude Code finishes | Sends notification to Flipper |

### Protocol

Communication happens over USB serial (115200 baud) via the Flipper CLI.

**Confirm request (PC → Flipper):**
```
claude_confirm <title>%1F<option1>%1F<option2>...\r\n
```
`%1F` is the URL-encoded unit separator (control characters are stripped by the Flipper CLI).

**Confirm response (Flipper → PC):**
```
CHOICE:<index>\r\n
```

**Notify (PC → Flipper):**
```
claude_notify <message>\r\n
```

### Exit Codes

| Exit code | Claude Code behavior |
|-----------|---------------------|
| `0` | Allow the tool to run |
| `2` | Block the tool |

### Session-wide Allow

Selecting "Yes, allow all bash (session)" creates `/tmp/.claude_flipper_always`. While this file exists, all subsequent Bash confirmations are automatically allowed without contacting the Flipper. Delete the file to re-enable per-command confirmation.

## Flipper App States

| State | Clawd animation | Monitor | Bottom text |
|-------|----------------|---------|-------------|
| Waiting | Typing (arms up/down) | Animated code lines | `Waiting...` |
| Confirm | Thinking pose | `?` | `Confirm?` |
| Complete | Arms raised | Blank | `Complete!` |

Complete state auto-resets to Waiting after ~3.5 seconds.

## Troubleshooting

**Flipper port not found**
```bash
ls /dev/tty.*        # check before and after plugging in USB
ls /dev/cu.usbmodem* # alternative path
```

**Timeout — command blocked**  
The hook waits up to 60 seconds for a response. If the Clawpper app is not running on the Flipper, the command will be blocked after timeout. Make sure the app is open before using Claude Code.

**ufbt launch fails**  
Try `ufbt build` first, then install `dist/clawpper.fap` manually via qFlipper.

**Reset session-wide allow**
```bash
rm /tmp/.claude_flipper_always
```

## License

[MIT](LICENSE)
