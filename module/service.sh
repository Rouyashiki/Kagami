#!/system/bin/sh

MODDIR="${0%/*}"
BASE_DIR="/data/adb/kagami"
mkdir -p "$BASE_DIR"
chmod 0755 "$MODDIR/kagamid" 2>/dev/null || true

"$MODDIR/kagamid" daemon start >/dev/null 2>&1 || true

exit 0
