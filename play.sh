#!/bin/sh
# Burnout Dominator Reversed - start the game (after tools/install.py).
cd "$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")" || exit 1
exec python3 tools/project.py run --status-ms 5000 --tail 80 "$@"
