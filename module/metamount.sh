#!/system/bin/sh
#
# Kagami metamodule — mount hook.
#
# KernelSU runs this after module initialization scripts and system.prop in
# both normal boot and late-load mode. Mounts use the "KSU" source so KernelSU
# can track, unload and hide them.

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
    set --
    if [ "${KSU_LATE_LOAD:-0}" = "1" ]; then
        set -- --late-load
    fi
    KAGAMI_ALLOW_DAEMON_START=1 "$MODDIR/kagamid" module mount-all "$@" >/dev/null 2>&1
fi

# Best-effort: notify KernelSU that module mounting has completed (matches
# other metamodules; harmless if the command is absent).
if [ -x /data/adb/ksud ]; then
    /data/adb/ksud kernel notify-module-mounted >/dev/null 2>&1 || true
fi

exit 0
