#!/bin/bash
# Run the simulator build. Creates/boots an "BotW iPhone 16 Pro" simulator if needed, installs the
# app and launches it with the Mac's game folder (no copy) and stdout/stderr on this terminal.
#   ios/scripts/sim.sh [extra runtime args, e.g. --seconds 60 --mute]
#   BOTW_SDK=xrsimulator ios/scripts/sim.sh ...   # Apple Vision Pro simulator ("BotW Apple Vision Pro")
set -euo pipefail
. "$(dirname "$0")/env.sh"
SDK="${BOTW_SDK:-iphonesimulator}"
APP="$ROOT/build-ios/$SDK/SheikahVision.app"
GAME="$BOTW_GAME"
NAME="BotW iPhone 16 Pro" TYPE=com.apple.CoreSimulator.SimDeviceType.iPhone-16-Pro PLATFORM=iOS
[ "$SDK" = "xrsimulator" ] && NAME="BotW Apple Vision Pro" TYPE=com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro-4K PLATFORM=xrOS
UDID=$(xcrun simctl list devices -j | uv run --no-project python -I -c "
import json,sys
d=json.load(sys.stdin)['devices']
print(next((x['udid'] for v in d.values() for x in v if x['name']=='$NAME' and x.get('isAvailable')),''))")
if [ -z "$UDID" ]; then
  RT=$(xcrun simctl list runtimes -j | uv run --no-project python -I -c "
import json,sys
print([r['identifier'] for r in json.load(sys.stdin)['runtimes'] if r['platform']=='$PLATFORM'][-1])")
  UDID=$(xcrun simctl create "$NAME" "$TYPE" "$RT")
fi
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl install "$UDID" "$APP"
exec xcrun simctl launch --console-pty --terminate-running-process "$UDID" "$BOTW_BUNDLE_ID" --game "$GAME" "$@"
