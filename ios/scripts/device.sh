#!/bin/bash
# Device helpers (iPhone connected by USB or on the same network, Developer Mode on).
#   ios/scripts/device.sh list                    # paired devices
#   ios/scripts/device.sh install                 # install build-ios/iphoneos/SheikahVision.app
#   ios/scripts/device.sh copy-game [dir]         # copy the game folder (default $BOTW_GAME) into Documents/game
#                                                 # (incremental: unchanged files are skipped)
#   ios/scripts/device.sh run [args...]           # launch with console output on this terminal
#   ios/scripts/device.sh log                     # fetch Library/Application Support/SheikahVision/runtime.log
# BOTW_DEVICE selects the device (UDID/name); default: the first paired iPhone.
# BOTW_SDK=xros: the Apple Vision Pro build (build-ios/xros) and the first paired Vision Pro.
set -euo pipefail
. "$(dirname "$0")/env.sh"
BID="$BOTW_BUNDLE_ID"
SDK="${BOTW_SDK:-iphoneos}"
KIND=iPhone
[ "$SDK" = "xros" ] && KIND=realityDevice
dev() {
  if [ -n "${BOTW_DEVICE:-}" ]; then echo "$BOTW_DEVICE"; return; fi
  local j; j=$(mktemp)
  xcrun devicectl list devices --json-output "$j" >/dev/null 2>&1
  uv run --no-project python -I -c "
import json,sys
d=json.load(open('$j'))['result']['devices']
# the CoreDevice identifier, not the UDID: with the UDID installs failed with CoreDeviceError 4016 (usage
# assertion) on an awake, unlocked headset; the identifier worked at once (2026-11-12)
ids=[x.get('identifier') or x['hardwareProperties'].get('udid') for x in d
     if x.get('hardwareProperties',{}).get('deviceType')=='$KIND' and x['hardwareProperties'].get('reality')=='physical']
print(ids[0] if ids else '')"
  rm -f "$j"
}
cmd="${1:-}"; shift || true
need_dev() { local d; d=$(dev); [ -n "$d" ] || { echo "no physical $KIND found (connect it, unlock it, trust this Mac)" >&2; return 1; }; echo "$d"; }
case "$cmd" in install|copy-game|run|log) D=$(need_dev) || exit 1 ;; esac
case "$cmd" in
  list) xcrun devicectl list devices ;;
  install)
    # say which app goes on, and warn when it is older than the last commit to the app's code (2026-11-17: a build for
    # the wrong platform left an old Vision Pro app here, and it was installed as if new)
    APP="$ROOT/build-ios/$SDK/SheikahVision.app"
    [ -d "$APP" ] || { echo "no $APP: build it first (ios/scripts/build.sh $SDK)" >&2; exit 1; }
    bin=$(stat -f %m "$APP/SheikahVision"); src=$(git -C "$ROOT" log -1 --format=%ct -- runtime ios 2>/dev/null || echo 0)
    echo "installing $SDK app: build $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Info.plist" 2>/dev/null), binary $(stat -f '%Sm' "$APP/SheikahVision")"
    [ "$bin" -ge "$src" ] || echo "warning: the app is older than the last commit to runtime/ or ios/: rebuild with ios/scripts/build.sh $SDK" >&2
    xcrun devicectl device install app --device "$D" "$APP" ;;
  copy-game)
    src="${1:-$BOTW_GAME}"
    time xcrun devicectl device copy to --device "$D" --domain-type appDataContainer --domain-identifier "$BID" \
      --source "$src" --destination Documents/game ;;
  run) xcrun devicectl device process launch --device "$D" --console --terminate-existing "$BID" "$@" ;;
  log)
    xcrun devicectl device copy from --device "$D" --domain-type appDataContainer --domain-identifier "$BID" \
      --source "Library/Application Support/SheikahVision/runtime.log" --destination "$ROOT/build-ios/runtime-device.log"
    echo "$ROOT/build-ios/runtime-device.log" ;;
  *) sed -n 2,10p "$0"; exit 2 ;;
esac
