#!/system/bin/sh

ui_print "- Installing Kagami"

ABI="$(getprop ro.product.cpu.abi 2>/dev/null)"
case "$ABI" in
    arm64-v8a|armeabi-v7a|x86|x86_64) BINARY_NAME="kagamid-$ABI" ;;
    *) abort "! Unsupported ABI: $ABI" ;;
esac

if [ -f "$MODPATH/$BINARY_NAME" ]; then
    cp "$MODPATH/$BINARY_NAME" "$MODPATH/kagamid"
elif [ -f "$MODPATH/kagamid" ]; then
    ui_print "- Using generic kagamid binary"
else
    abort "! Binary not found: $BINARY_NAME"
fi
chmod 0755 "$MODPATH/kagamid"
[ -f "$MODPATH/lkmloader" ] || abort "! lkmloader missing"
chmod 0755 "$MODPATH/lkmloader"

rm -f "$MODPATH"/kagamid-arm64-v8a \
      "$MODPATH"/kagamid-armeabi-v7a \
      "$MODPATH"/kagamid-x86 \
      "$MODPATH"/kagamid-x86_64

BASE_DIR="/data/adb/kagami"
mkdir -p "$BASE_DIR"

if [ ! -f "$BASE_DIR/config.json" ]; then
    "$MODPATH/kagamid" config gen -o "$BASE_DIR/config.json" || abort "! Failed to generate config"
fi

for ROOT_IMPL in /data/adb/ksu /data/adb/ap; do
    if [ -d "$ROOT_IMPL" ]; then
        mkdir -p "$ROOT_IMPL/bin"
        ln -sf /data/adb/modules/kagami/kagamid "$ROOT_IMPL/bin/kagamid"
    fi
done

ui_print "- Kagami installed"
