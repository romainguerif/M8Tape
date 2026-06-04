#!/bin/sh
# M8Tape Studio - graphical sampling utility (SDL2). Pak entry point.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

# FAT32/exFAT cards don't store the exec bit; re-set it each launch.
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

# --- audio diagnostics only (NO disk mount here — that could block) ----------
sleep 2
DIAG="$PAK_DIR/diag.txt"
{
    echo "=== date ==="; date 2>/dev/null
    echo "=== arch / libc ==="; uname -a 2>/dev/null
    echo "=== arecord -l ==="; arecord -l 2>&1
    echo "=== /proc/asound/cards ==="; cat /proc/asound/cards 2>&1
    for s in /proc/asound/card*/stream0; do
        [ -f "$s" ] || continue
        echo "=== $s ==="; cat "$s" 2>&1
    done
} > "$DIAG" 2>&1

# M8 disk ENUMERATION probe — SAFE: backgrounded (& so it never blocks the app)
# and reads ONLY /proc + /sys + dmesg. No mount, no /dev I/O → cannot wedge.
(
    sleep 3
    echo "=== filesystems ==="; cat /proc/filesystems 2>/dev/null
    echo "=== storage/fs modules ==="; grep -iE "usb_storage|usb-storage|vfat|exfat|nls" /proc/modules 2>/dev/null
    echo "=== /proc/partitions ==="; cat /proc/partitions 2>/dev/null
    echo "=== sd block devices ==="
    for d in /sys/block/sd*; do [ -e "$d" ] || continue
        echo "$(basename "$d"): removable=$(cat "$d/removable" 2>/dev/null) size=$(cat "$d/size" 2>/dev/null) vendor=$(cat "$d/device/vendor" 2>/dev/null) model=$(cat "$d/device/model" 2>/dev/null)"
    done
    echo "=== usb ids of sd devices ==="
    for d in /sys/block/sd*; do [ -e "$d" ] || continue
        real="$(readlink -f "$d/device" 2>/dev/null)"; p="$real"
        while [ -n "$p" ] && [ "$p" != "/" ]; do
            [ -f "$p/idVendor" ] && { echo "$(basename "$d"): $(cat "$p/idVendor" 2>/dev/null):$(cat "$p/idProduct" 2>/dev/null) ($(cat "$p/product" 2>/dev/null))"; break; }
            p="$(dirname "$p")"
        done
    done
    echo "=== dmesg tail ==="; dmesg 2>/dev/null | tail -40
    echo "=== done ==="
) > "$PAK_DIR/m8disk.txt" 2>&1 &

# bundle our libmsettings.so (system SDL2/ttf/image are provided by NextUI).
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

"$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR" >"$PAK_DIR/m8tape.log" 2>&1
echo "exit code: $?" >> "$PAK_DIR/m8tape.log"
