#!/bin/bash
# Configure + build + sign the iOS app into build-ios/<sdk>/SheikahVision.app.
#   ios/scripts/build.sh iphoneos          # device (needs a provisioning profile, see provision.sh)
#   ios/scripts/build.sh iphonesimulator   # simulator
#   ios/scripts/build.sh xros              # Apple Vision Pro
#   BOTW_SDK=xros ios/scripts/build.sh     # the same (as device.sh / provision.sh / sim.sh take it)
# With neither, it builds for iphoneos: 2026-11-17 `BOTW_SDK=xros build.sh` was ignored here, the iPhone app was built
# and the old Vision Pro app installed twice. Now both work, a mismatch stops, and the platform is printed.
# Env: BOTW_IOS_JOBS (default 6), BOTW_PAID=1 (sign with extended-virtual-addressing too).
set -euo pipefail
. "$(dirname "$0")/env.sh"
if [ -n "${1:-}" ] && [ -n "${BOTW_SDK:-}" ] && [ "$1" != "$BOTW_SDK" ]; then
  echo "build.sh: argument $1 and BOTW_SDK=$BOTW_SDK disagree" >&2; exit 1
fi
SDK="${1:-${BOTW_SDK:-iphoneos}}"
echo "build.sh: building for $SDK ($(botw_system "$SDK")) into build-ios/$SDK"
B="$ROOT/build-ios/$SDK"
[ -d "$BOTW_IOS_DEPS/prefix-$SDK" ] || "$ROOT/ios/scripts/build_deps.sh" "$SDK"
# the build number and source commit shown in the launcher (ios/CMakeLists.txt); env BOTW_BUILD_NUMBER / BOTW_SOURCE_COMMIT override them
BUILD_NUMBER="${BOTW_BUILD_NUMBER:-$(date +%Y%m%d%H%M)}"
if [ -z "${BOTW_SOURCE_COMMIT:-}" ]; then
  BOTW_SOURCE_COMMIT=$(git -C "$ROOT" rev-parse --short=8 HEAD 2>/dev/null || echo dev)
  if [ "$BOTW_SOURCE_COMMIT" != dev ] && [ -n "$(git -C "$ROOT" status --porcelain -- runtime ios cafe tools CMakeLists.txt VERSION 2>/dev/null)" ]; then
    BOTW_SOURCE_COMMIT="$BOTW_SOURCE_COMMIT+"
  fi
fi
cmake -S "$ROOT/ios" -B "$B" -G Ninja -DCMAKE_SYSTEM_NAME="$(botw_system "$SDK")" -DCMAKE_OSX_SYSROOT="$SDK" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$(botw_min_os "$SDK")" \
  -DBOTW_BUNDLE_ID="$BOTW_BUNDLE_ID" -DBOTW_IOS_DEPS="$BOTW_IOS_DEPS" \
  -DBOTW_BUILD_NUMBER="$BUILD_NUMBER" -DBOTW_SOURCE_COMMIT="$BOTW_SOURCE_COMMIT" >/dev/null
nice -n 10 cmake --build "$B" -j "${BOTW_IOS_JOBS:-6}"
"$ROOT/ios/scripts/sign.sh" "$SDK"
APP="$B/SheikahVision.app"
echo "build.sh: $SDK app $APP, binary $(stat -f '%Sm' "$APP/SheikahVision"), build $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Info.plist" 2>/dev/null)"
