#!/bin/bash
# Local ad-hoc Mac app only. No Developer ID signing, notarization, DMG or IPA.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/build-installer/SheikahVision Installer.app"
swift build --package-path "$ROOT/installer/app" -c release
BIN="$(swift build --package-path "$ROOT/installer/app" -c release --show-bin-path)"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/SheikahInstaller" "$APP/Contents/MacOS/SheikahInstaller"
# no debug map or local symbols: they carry this Mac's build paths (and so its user name)
xcrun strip -S -x "$APP/Contents/MacOS/SheikahInstaller"
ditto "$BIN/SheikahInstaller_SheikahInstaller.bundle" "$APP/Contents/Resources/SheikahInstaller_SheikahInstaller.bundle"
# app icon: the headset app's icon (Resources/logo.png, 512 px round)
ICONSET="$(mktemp -d)/AppIcon.iconset"; mkdir -p "$ICONSET"
LOGO="$ROOT/installer/app/Sources/SheikahInstaller/Resources/logo.png"
for s in 16 32 128 256; do
  sips -z $s $s "$LOGO" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  sips -z $((s * 2)) $((s * 2)) "$LOGO" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
sips -z 512 512 "$LOGO" --out "$ICONSET/icon_512x512.png" >/dev/null
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$(dirname "$ICONSET")"
xcrun python3 "$ROOT/installer/scripts/bundle_source.py" "$ROOT" "$APP/Contents/Resources/source"
KIT="${SV_RUNTIME_KIT:-$ROOT/build-installer/runtime-kit}"
if [ -f "$KIT/runtime.json" ]; then
  xcrun python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import runtimekit; runtimekit.verify(sys.argv[2])' "$ROOT/installer/engine" "$KIT"
  STAGED_KIT="$(mktemp -d "$ROOT/build-installer/runtime-stage.XXXXXX")"
  ditto "$KIT" "$STAGED_KIT"
  rm -rf "$APP/Contents/Resources/runtime-kit"
  mv "$STAGED_KIT" "$APP/Contents/Resources/runtime-kit"
else
  rm -rf "$APP/Contents/Resources/runtime-kit"
fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.botw.sheikahvision.installer</string>
<key>CFBundleExecutable</key><string>SheikahInstaller</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>CFBundleName</key><string>SheikahVision Installer</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>${SV_INSTALLER_VERSION:-$(tr -d '[:space:]' < "$ROOT/VERSION")}</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - --timestamp=none "$APP"
codesign --verify --strict "$APP"
echo "$APP"
