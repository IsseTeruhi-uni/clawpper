#!/bin/bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
CLAUDE_DIR="$HOME/.claude"
SETTINGS="$CLAUDE_DIR/settings.json"

mkdir -p "$CLAUDE_DIR"

# Copy bridge script
cp "$REPO_DIR/bridge/flipper_bridge.py" "$CLAUDE_DIR/flipper_bridge.py"
chmod +x "$CLAUDE_DIR/flipper_bridge.py"
echo "✓ flipper_bridge.py -> $CLAUDE_DIR/"

# Merge hooks into settings.json
if [ ! -f "$SETTINGS" ]; then
    echo '{}' > "$SETTINGS"
fi

python3 - <<EOF
import json

with open('$SETTINGS') as f:
    current = json.load(f)

with open('$REPO_DIR/.claude/settings.json') as f:
    src = json.load(f)

hooks = current.setdefault('hooks', {})
for event, entries in src.get('hooks', {}).items():
    existing = hooks.setdefault(event, [])
    # Skip if the same command is already registered
    existing_cmds = {h['command'] for e in existing for h in e.get('hooks', [])}
    for entry in entries:
        new_cmds = {h['command'] for h in entry.get('hooks', [])}
        if not new_cmds & existing_cmds:
            existing.append(entry)

with open('$SETTINGS', 'w') as f:
    json.dump(current, f, indent=2, ensure_ascii=False)
    f.write('\n')

print('✓ hooks merged into $SETTINGS')
EOF
