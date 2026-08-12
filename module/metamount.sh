#!/system/bin/sh
#
# Kagami metamodule — mount hook.
#
# KernelSU runs this at the post-fs-data stage, after every module's
# post-fs-data.sh and before zygote starts, in the init mount namespace. This
# is where a metamodule mounts all enabled modules. All mounts use the "KSU"
# source so KernelSU can track / unload / hide them.

MODDIR="${0%/*}"
BASE_DIR="/data/adb/kagami"
LOG_FILE="$BASE_DIR/daemon.log"

mkdir -p "$BASE_DIR" "$BASE_DIR/run"
# Keep daemon.log scoped to this boot's mount attempt. Android normally has not
# synchronized wall-clock time at post-fs-data; kagamid labels those lines as
# early-boot instead of inventing a 1970 date.
: >"$LOG_FILE"
chmod 0755 "$MODDIR/kagamid" 2>/dev/null || true

if [ -x "$MODDIR/kagamid" ]; then
    KAGAMI_ALLOW_DAEMON_START=1 "$MODDIR/kagamid" module mount-all >/dev/null 2>&1
fi

# Best-effort: notify KernelSU that module mounting has completed (matches
# other metamodules; harmless if the command is absent).
if [ -x /data/adb/ksud ]; then
    /data/adb/ksud kernel notify-module-mounted >/dev/null 2>&1 || true
fi

exit 0
