#!/usr/bin/env bash
#
# Build kagamid (Android) and package the Kagami metamodule into a
# KernelSU-flashable zip. Install with:  ksud module install <zip>
#
# Usage:
#   scripts/package.sh                      # arm64-v8a (default)
#   ABI=x86_64 scripts/package.sh           # another ABI
#   ANDROID_NDK_HOME=/path scripts/package.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ABI="${ABI:-arm64-v8a}"
PLATFORM="${ANDROID_PLATFORM:-android-26}"
BUILD_DIR="${BUILD_DIR:-build-android-${ABI}}"

# --- locate the Android NDK ---------------------------------------------------
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ]; then
  for d in "$HOME/Library/Android/sdk/ndk"/* "$HOME/Android/Sdk/ndk"/* /opt/android-ndk*; do
    [ -d "$d" ] && NDK="$d"
  done
fi
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "error: Android NDK not found; set ANDROID_NDK_HOME" >&2
  exit 1
fi
echo "==> NDK:      $NDK"
echo "==> ABI:      $ABI ($PLATFORM)"

# --- configure (once) + build kagamid ----------------------------------------
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$PLATFORM" \

fi
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" --target kagamid lkmloader -j"$JOBS"

BIN="$BUILD_DIR/kagamid-${ABI}"
LOADER="$BUILD_DIR/third_party/lkmloader/lkmloader"
[ -f "$BIN" ] || { echo "error: built binary $BIN missing" >&2; exit 1; }
[ -f "$LOADER" ] || { echo "error: built binary $LOADER missing" >&2; exit 1; }

# --- module metadata ----------------------------------------------------------
ID="$(sed -n 's/^id=//p' module/module.prop | head -1)"
VER="$(sed -n 's/^version=//p' module/module.prop | head -1)"
: "${ID:?missing id in module.prop}"

# --- stage the static WebUI (webui/ source -> module/webroot artifact) -------
rm -rf module/webroot && cp -r webui module/webroot

# --- stage the module tree (module/ + ABI binary) ----------------------------
PKG="$BUILD_DIR/package"
OUT="$BUILD_DIR/out"
rm -rf "$PKG"
mkdir -p "$PKG" "$OUT"
cp -R "module/." "$PKG/"
cp "$BIN" "$PKG/kagamid-${ABI}"        # customize.sh picks the right ABI at install
cp "$LOADER" "$PKG/lkmloader"
find "$PKG" -name '.DS_Store' -delete 2>/dev/null || true
chmod 0755 "$PKG/kagamid-${ABI}"
chmod 0755 "$PKG/lkmloader"
chmod 0644 "$PKG/module.prop"
find "$PKG" -maxdepth 1 -name '*.sh' -exec chmod 0755 {} +

# --- zip (ksud-flashable: module.prop + scripts at the zip root) --------------
ZIP="$ROOT/$OUT/${ID}-${VER:-dev}-${ABI}.zip"   # absolute: zip runs from inside $PKG
rm -f "$ZIP"
( cd "$PKG" && zip -r -X -q "$ZIP" . -x '.*' )
echo "==> packaged: $ZIP"
unzip -l "$ZIP" | awk 'NR==1||/module.prop|metamount|metainstall|metauninstall|customize|kagamid-|lkmloader/' || true
