#!/bin/bash
# Build a redistributable visionOS runtime kit, without requiring generated/ or any game files. Paths of the build,
# dependency and source folders are remapped so the kit names no local folder or user (runtimekit.py checks).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/build-installer/runtime-kit}"
BUILD="${SV_RUNTIME_BUILD:-$ROOT/build-installer/runtime-only}"
DEPS="${SV_RUNTIME_DEPS:-$ROOT/ios-deps}"
JOBS="${SV_JOBS:-$(getconf NPROCESSORS_ONLN)}"
xcrun python3 "$ROOT/installer/scripts/check_source.py" "$ROOT" "$BUILD"
[ -d "$DEPS/prefix-xros" ] || BOTW_IOS_DEPS="$DEPS" "$ROOT/ios/scripts/build_deps.sh" xros
cmake -S "$ROOT/ios" -B "$BUILD" -G Ninja -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DBOTW_RUNTIME_ONLY=ON \
  -DBOTW_GENERATED_DIR="$BUILD/no-game-sources" -DBOTW_IOS_DEPS="$DEPS" \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=$BUILD=build -ffile-prefix-map=$DEPS=deps -ffile-prefix-map=$ROOT=." \
  -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$BUILD=build -ffile-prefix-map=$DEPS=deps -ffile-prefix-map=$ROOT=." \
  -DCMAKE_OBJC_FLAGS="-ffile-prefix-map=$BUILD=build -ffile-prefix-map=$DEPS=deps -ffile-prefix-map=$ROOT=." \
  -DCMAKE_OBJCXX_FLAGS="-ffile-prefix-map=$BUILD=build -ffile-prefix-map=$DEPS=deps -ffile-prefix-map=$ROOT=." \
  -DCMAKE_Swift_FLAGS="-debug-prefix-map $BUILD=build -debug-prefix-map $ROOT=."
cmake --build "$BUILD" --target sheikah_runtime botw_immersive -j "$JOBS"
# Fail closed if an unexpected guest object ever reaches the public archive.
if /usr/bin/ar -t "$BUILD/libsheikah_runtime.a" | /usr/bin/grep -E '^(fn_|big_|huge_|table\.cpp|hle_stubs)'; then
  echo 'refusing to export a runtime archive containing generated objects' >&2
  exit 1
fi
xcrun python3 "$ROOT/installer/engine/runtimekit.py" export "$BUILD" "$OUT"
echo "Public runtime kit: $OUT"
