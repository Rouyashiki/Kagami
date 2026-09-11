#!/system/bin/sh

MODDIR="${0%/*}"
BASE_DIR="/data/adb/kagami"

mkdir -p "$BASE_DIR"
mkdir -p "$BASE_DIR/run"

chmod 0755 "$MODDIR/kagamid" 2>/dev/null || true
"$MODDIR/kagamid" prepare "$MODDIR" || exit 1

if [ ! -f "$BASE_DIR/config.json" ]; then
    "$MODDIR/kagamid" config gen -o "$BASE_DIR/config.json" >/dev/null 2>&1 || true
fi

exit 0
