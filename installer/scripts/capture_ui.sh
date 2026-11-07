#!/bin/bash
# Synthetic UI only: no engine command, signing operation, or headset connection.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
swift build --package-path "$ROOT/installer/app"
BIN="$(swift build --package-path "$ROOT/installer/app" --show-bin-path)"
for lang in zh en; do
  SV_UI_LANGUAGE="$lang" SV_UI_CAPTURE_DIR="$ROOT/docs/images/installer-redesign/$lang" \
    "$BIN/SheikahInstaller" --capture-ui
done
