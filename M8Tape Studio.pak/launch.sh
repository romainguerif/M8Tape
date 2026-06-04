#!/bin/sh
# M8Tape Studio - graphical sampling utility (SDL2). Pak entry point.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

# FAT32/exFAT cards don't store the exec bit; re-set it each launch.
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

# --- diagnostics (so we can validate detection remotely over HTTP) ----------
# wait so a freshly-plugged USB audio OR mass-storage device has time to enumerate
sleep 4
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
    for i in /proc/asound/card*/id; do
        [ -f "$i" ] || continue
        echo "=== $i ==="; cat "$i" 2>&1
    done
    echo "=== SDL libs present ==="; ls -1 /usr/lib/ 2>/dev/null | grep -i sdl

    # --- M8 USB-Drive-mode probe (READ-ONLY, no writes to the M8 card) ---
    echo "=== M8 DISK PROBE ==="
    echo "--- /proc/filesystems ---"; cat /proc/filesystems 2>/dev/null
    echo "--- storage/fs modules ---"; grep -iE "usb_storage|usb-storage|vfat|exfat|fat|nls" /proc/modules 2>/dev/null || echo "(none listed / built-in)"
    echo "--- tools ---"; for t in mount umount blkid fsck.fat fsck.exfat; do printf '%s: ' "$t"; command -v "$t" 2>/dev/null || echo MISSING; done
    echo "--- /proc/partitions ---"; cat /proc/partitions 2>/dev/null
    echo "--- sd block devices ---"
    for d in /sys/block/sd*; do [ -e "$d" ] || continue; echo "$(basename "$d"): vendor='$(cat "$d/device/vendor" 2>/dev/null)' model='$(cat "$d/device/model" 2>/dev/null)' removable=$(cat "$d/removable" 2>/dev/null)"; done
    echo "--- blkid ---"; for p in /dev/sd*; do [ -e "$p" ] || continue; printf '%s -> ' "$p"; blkid "$p" 2>/dev/null || echo "(no blkid)"; done
    echo "--- dmesg tail ---"; dmesg 2>/dev/null | tail -20
    PART="$(ls /dev/sd*1 2>/dev/null | head -1)"; [ -z "$PART" ] && PART="$(ls /dev/sda 2>/dev/null | head -1)"
    if [ -n "$PART" ]; then
        echo "--- READ-ONLY mount test: $PART ---"; mkdir -p /tmp/m8probe 2>/dev/null
        if mount -o ro "$PART" /tmp/m8probe 2>&1; then
            echo "mounted ro OK"; df -h /tmp/m8probe 2>/dev/null
            [ -d /tmp/m8probe/Samples ] && echo "YES /Samples exists" || echo "NO /Samples"
            ls /tmp/m8probe/Samples 2>/dev/null | head -40
            sync; umount /tmp/m8probe 2>&1 && echo "unmounted cleanly" || echo "UMOUNT FAILED"
        else
            echo "auto mount ro failed; trying explicit fs:"
            mount -t vfat  -o ro "$PART" /tmp/m8probe 2>&1 && { echo vfat-ro-OK;  ls /tmp/m8probe | head; sync; umount /tmp/m8probe; } || echo vfat-ro-fail
            mount -t exfat -o ro "$PART" /tmp/m8probe 2>&1 && { echo exfat-ro-OK; ls /tmp/m8probe | head; sync; umount /tmp/m8probe; } || echo exfat-ro-fail
        fi
    else
        echo "--- no /dev/sd* (M8 not in USB_DRIVE mode, or not enumerated) ---"
    fi
} > "$DIAG" 2>&1

# bundle our libmsettings.so (system SDL2/ttf/image are provided by NextUI).
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

"$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR" >"$PAK_DIR/m8tape.log" 2>&1
echo "exit code: $?" >> "$PAK_DIR/m8tape.log"
