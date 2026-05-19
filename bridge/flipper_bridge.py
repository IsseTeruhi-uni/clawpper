#!/usr/bin/env python3
import os
import sys
import glob
import time
import termios
import select
import json

BAUD = termios.B115200
TIMEOUT = 30
ALLOW_ALWAYS_FLAG = "/tmp/.claude_flipper_always"
SEP = "%1F"  # URL-encoded unit separator (control chars are stripped by Flipper CLI)


def find_flipper_port():
    for pattern in [
        '/dev/tty.usbmodemflip*',
        '/dev/tty.usbmodem*',
        '/dev/cu.usbmodem*',
        '/dev/tty.Flipper*',
        '/dev/cu.Flipper*',
    ]:
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None


def open_serial(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                             # iflag
    attrs[1] = 0                                             # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL # cflag: 8N1
    attrs[3] = 0                                             # lflag: raw
    attrs[4] = BAUD                                          # ispeed
    attrs[5] = BAUD                                          # ospeed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 10
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def log(msg):
    print(f"[flipper_bridge] {msg}", file=sys.stderr)


def drain(fd, timeout=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            break
        os.read(fd, 256)


def read_stdin_json():
    try:
        r, _, _ = select.select([sys.stdin], [], [], 0.5)
        if r:
            return json.loads(sys.stdin.read())
    except Exception:
        pass
    return {}


def build_confirm(stdin_data):
    """Build a title and option list based on the tool being confirmed.
    Each option is a (label, action) tuple.
    Actions: "allow" | "allow_always" | "deny"
    """
    tool_name = stdin_data.get("tool_name", "")
    tool_input = stdin_data.get("tool_input", {})

    # Fallback to env var if tool_input is empty
    if not tool_input:
        try:
            tool_input = json.loads(os.environ.get("CLAUDE_TOOL_INPUT", "{}"))
        except Exception:
            tool_input = {}

    if tool_name == "Bash" or "command" in tool_input:
        cmd = tool_input.get("command", "")[:48]
        title = f"Run: {cmd}"
        options = [
            ("Yes",                          "allow"),
            ("Yes, allow all bash (session)", "allow_always"),
            ("No",                           "deny"),
        ]
    elif tool_name in ("Write", "Edit", "NotebookEdit") or "file_path" in tool_input:
        path = tool_input.get("file_path", "")
        fname = os.path.basename(path)[:36]
        title = f"Edit: {fname}"
        options = [
            ("Yes",                              "allow"),
            ("Yes, allow file edits (session)",  "allow_always"),
            ("No",                               "deny"),
        ]
    elif tool_name == "WebFetch" or "url" in tool_input:
        url = tool_input.get("url", "")[:40]
        title = f"Fetch: {url}"
        options = [
            ("Yes", "allow"),
            ("No",  "deny"),
        ]
    else:
        title = tool_name or "Action"
        options = [
            ("Yes", "allow"),
            ("No",  "deny"),
        ]

    return title, options


def build_notify_message(stdin_data):
    transcript = stdin_data.get("transcript_path", "")
    if transcript and os.path.exists(transcript):
        last = _last_assistant_text(transcript)
        if last:
            return last[:60]
    return "Done!"


def _last_assistant_text(path):
    last = None
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    entry = json.loads(line)
                    if entry.get("role") == "assistant":
                        for block in entry.get("content", []):
                            if isinstance(block, dict) and block.get("type") == "text":
                                last = block["text"]
                            elif isinstance(block, str):
                                last = block
                except Exception:
                    pass
    except Exception:
        pass
    return last


def send_and_wait(mode, title, options=None):
    port = find_flipper_port()
    if not port:
        log("Flipper port not found — skipping")
        sys.exit(0)

    log(f"Found port: {port}")

    try:
        fd = open_serial(port)
        time.sleep(0.3)
        drain(fd)

        if mode == "NOTIFY":
            cmd = f"claude_notify {title}\r\n"
            log(f"Sending: {cmd.strip()}")
            os.write(fd, cmd.encode())
            time.sleep(1.0)
            os.close(fd)
            sys.exit(0)

        # Check session-wide allow flag
        if os.path.exists(ALLOW_ALWAYS_FLAG):
            os.close(fd)
            log("Allow-always flag set — skipping Flipper")
            sys.exit(0)

        # Send: title%1Foption1%1Foption2...
        labels = [opt[0] for opt in options]
        payload = title + SEP + SEP.join(labels)
        cmd = f"claude_confirm {payload}\r\n"
        log(f"Sending confirm (title={title!r}, opts={labels})")
        os.write(fd, cmd.encode())

        log(f"Waiting for CHOICE:n (timeout={TIMEOUT}s)...")
        buf = b""
        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], min(1.0, deadline - time.time()))
            if r:
                chunk = os.read(fd, 64)
                if chunk:
                    log(f"Received: {chunk!r}")
                    buf += chunk
                    for i, (label, action) in enumerate(options):
                        if f"CHOICE:{i}".encode() in buf:
                            os.close(fd)
                            log(f"Choice {i} ({label}) → {action}")
                            if action == "allow_always":
                                open(ALLOW_ALWAYS_FLAG, "w").close()
                                sys.exit(0)
                            elif action == "allow":
                                sys.exit(0)
                            else:
                                sys.exit(2)

        os.close(fd)
        log("Timeout — blocking by default")
        sys.exit(2)

    except Exception as e:
        log(f"Exception: {e} — allowing")
        sys.exit(0)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "NOTIFY"
    stdin_data = read_stdin_json()

    if mode == "CONFIRM":
        title, options = build_confirm(stdin_data)
        send_and_wait("CONFIRM", title, options)
    else:
        title = build_notify_message(stdin_data)
        send_and_wait("NOTIFY", title)
