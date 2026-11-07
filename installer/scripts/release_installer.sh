#!/bin/bash
# Release build of the Mac installer app for players.
#
#   installer/scripts/release_installer.sh
#
# Default (decided 2026-11-11): ad-hoc signature (codesign --sign -), no Developer ID, no notarization, so the app
# carries no developer team, company or certificate. Players open it once with System Settings › Privacy & Security ›
# Open Anyway (see installer/README.md). Their headset builds are still signed with each player's own Apple account.
#
# The script refuses to package an app that mentions any team ID or team name known to this Mac's Xcode, any
# signing identity in this keychain, or this Mac's home folder.
#
# Optional, off unless both are set explicitly: SV_RELEASE_SIGNING=developer-id SV_DEVELOPER_ID="Developer ID
# Application: ..." signs with the hardened runtime instead; adding --submit with SV_NOTARY_PROFILE uploads to Apple's
# notary service. That path names the signing team in the app, so it is not used for the public installer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/build-installer/SheikahVision Installer.app"
OUT="${SV_RELEASE_DIR:-$ROOT/build-installer/release}"
MODE="${SV_RELEASE_SIGNING:-adhoc}"
SUBMIT=0
[ "${1:-}" = "--submit" ] && SUBMIT=1

case "$MODE" in
  adhoc) [ "$SUBMIT" = 0 ] || { echo "--submit needs SV_RELEASE_SIGNING=developer-id." >&2; exit 2; } ;;
  developer-id)
    case "${SV_DEVELOPER_ID:-}" in
      "Developer ID Application:"*) ;;
      *) echo "SV_RELEASE_SIGNING=developer-id needs SV_DEVELOPER_ID=\"Developer ID Application: ...\"." >&2; exit 2 ;;
    esac
    security find-identity -v -p codesigning | grep -qF "\"$SV_DEVELOPER_ID\"" || { echo "No such identity: $SV_DEVELOPER_ID" >&2; exit 2; }
    if [ "$SUBMIT" = 1 ] && [ -z "${SV_NOTARY_PROFILE:-}" ]; then echo "--submit needs SV_NOTARY_PROFILE." >&2; exit 2; fi ;;
  *) echo "SV_RELEASE_SIGNING is adhoc (default) or developer-id." >&2; exit 2 ;;
esac

# Players get the prebuilt runtime kit; a release without it would make every player build Cemu from source.
KIT="${SV_RUNTIME_KIT:-$ROOT/build-installer/runtime-kit}"
[ -f "$KIT/runtime.json" ] || { echo "No runtime kit at $KIT: run installer/scripts/prebuild_runtime.sh first." >&2; exit 2; }
# The bundled source and the kit must come from committed source (same check the kit export uses).
xcrun python3 "$ROOT/installer/scripts/check_source.py" "$ROOT" "$ROOT/build-installer/no-runtime-cache"

SV_RUNTIME_KIT="$KIT" "$ROOT/installer/scripts/build_installer.sh" >/dev/null   # ad-hoc signed already

if [ "$MODE" = developer-id ]; then
  codesign --force --options runtime --timestamp --sign "$SV_DEVELOPER_ID" "$APP"
else
  codesign --force --sign - --timestamp=none "$APP"
  # an ad-hoc signature has no team and no certificate chain
  INFO="$(codesign -dvvv "$APP" 2>&1)"
  echo "$INFO" | grep -q "^Signature=adhoc" || { echo "expected an ad-hoc signature" >&2; exit 1; }
  echo "$INFO" | grep -q "^TeamIdentifier=not set" || { echo "the signature names a team" >&2; exit 1; }
  if echo "$INFO" | grep -q "^Authority="; then echo "the signature carries a certificate" >&2; exit 1; fi
fi
codesign --verify --strict --deep "$APP"

# Leak check: terms collected from this Mac at run time, never written down here.
TERMS="$(mktemp)"; trap 'rm -f "$TERMS"' EXIT
{
  xcrun python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import sv
for i, name, _ in sv.xcode_teams() or []:
    print(i); print(name)' "$ROOT/installer/engine" 2>/dev/null || true
  security find-identity -v -p codesigning | sed -nE 's/.*"[^:]+: (.*) \(([A-Z0-9]{10})\)"$/\1\n\2/p'
  security find-identity -v -p codesigning | sed -nE 's/.*\(([A-Z0-9]{10})\)\)?"$/\1/p'
  echo "/Users/$(id -un)"; echo "$HOME"   # paths only: the public GitHub owner name in the releases link is intended
} | awk 'length($0) >= 4' | sort -u > "$TERMS"
if [ "$MODE" = adhoc ] && LC_ALL=C grep -rlaiF -f "$TERMS" "$APP" >/dev/null; then
  echo "The app mentions a team, signing identity or local user. Matches:" >&2
  LC_ALL=C grep -rlaiF -f "$TERMS" "$APP" >&2
  exit 1
fi
echo "Leak check passed ($(wc -l < "$TERMS" | tr -d ' ') local terms)."

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
mkdir -p "$OUT"
ZIP="$OUT/SheikahVision-Installer-$VERSION.zip"
rm -f "$ZIP"
ditto -c -k --norsrc --noextattr --keepParent "$APP" "$ZIP"

if [ "$SUBMIT" = 1 ]; then
  xcrun notarytool submit "$ZIP" --keychain-profile "$SV_NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  spctl --assess --type execute --verbose=4 "$APP"
  rm -f "$ZIP"; ditto -c -k --norsrc --noextattr --keepParent "$APP" "$ZIP"
fi
shasum -a 256 "$ZIP"
echo "Release zip ($MODE): $ZIP"
