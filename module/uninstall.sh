#!/system/bin/sh

MODDIR="${0%/*}"

# The daemon is the only persistent Kasumi FD owner. Ask it to unload Kagami's
# packaged LKM first, then stop it even when the LKM is external or built in.
if [ -x "$MODDIR/kagamid" ]; then
    "$MODDIR/kagamid" lkm unload >/dev/null 2>&1 || true
    "$MODDIR/kagamid" daemon stop >/dev/null 2>&1 || true
fi

rm -f /data/adb/ksu/bin/kagamid /data/adb/ap/bin/kagamid
exit 0
