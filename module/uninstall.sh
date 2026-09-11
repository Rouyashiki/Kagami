#!/system/bin/sh

MODDIR="${0%/*}"

# The daemon is the only persistent Kasumi FD owner. Ask it to unload Kagami's
# packaged LKM first, then stop it even when the LKM is external or built in.
if [ -x "$MODDIR/kagamid" ]; then
    "$MODDIR/kagamid" lkm unload >/dev/null 2>&1 || true
    "$MODDIR/kagamid" daemon stop >/dev/null 2>&1 || true
fi

for LINK in /data/adb/ksu/bin/kagamid /data/adb/ap/bin/kagamid; do
    if [ "$(readlink "$LINK" 2>/dev/null)" = "$MODDIR/kagamid" ]; then
        rm -f "$LINK"
    fi
done
exit 0
