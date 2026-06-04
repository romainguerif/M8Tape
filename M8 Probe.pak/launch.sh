#!/bin/sh
# M8 Probe - READ-ONLY diagnostic for M8 USB Drive mode. Writes m8probe.txt.
# Does NOT write to the M8 card. Mounts read-only, lists /Samples, unmounts.
PAK_DIR="$(dirname "$0")"
OUT="$PAK_DIR/m8probe.txt"
MP="/tmp/m8probe"

# give the mass-storage device time to enumerate after entering USB_DRIVE mode
sleep 3

{
    echo "=== date ==="; date 2>/dev/null
    echo "=== uname ==="; uname -a 2>/dev/null

    echo "=== /proc/filesystems (need vfat and/or exfat) ==="; cat /proc/filesystems 2>/dev/null
    echo "=== storage/fs modules ==="; grep -iE "usb_storage|usb-storage|vfat|exfat|fat|nls" /proc/modules 2>/dev/null || echo "(none listed; may be built-in)"
    echo "=== tools ==="; for t in mount umount blkid fsck.fat fsck.vfat fsck.exfat sync; do printf '%s: ' "$t"; command -v "$t" 2>/dev/null || echo "MISSING"; done

    echo "=== /proc/partitions ==="; cat /proc/partitions 2>/dev/null
    echo "=== block devices (sd*) ==="
    for d in /sys/block/sd*; do
        [ -e "$d" ] || continue
        echo "$d: vendor='$(cat "$d/device/vendor" 2>/dev/null)' model='$(cat "$d/device/model" 2>/dev/null)' removable=$(cat "$d/removable" 2>/dev/null) size=$(cat "$d/size" 2>/dev/null)"
    done

    echo "=== blkid (filesystem type of each sd partition) ==="
    for p in /dev/sd*; do [ -e "$p" ] || continue; printf '%s -> ' "$p"; blkid "$p" 2>/dev/null || echo "(blkid failed/absent)"; done

    echo "=== usb ids of sd devices ==="
    for d in /sys/block/sd*; do
        [ -e "$d" ] || continue
        real="$(readlink -f "$d/device" 2>/dev/null)"
        # walk up to a node that has idVendor
        p="$real"
        while [ -n "$p" ] && [ "$p" != "/" ]; do
            if [ -f "$p/idVendor" ]; then
                echo "$(basename "$d"): idVendor=$(cat "$p/idVendor" 2>/dev/null) idProduct=$(cat "$p/idProduct" 2>/dev/null) ($(cat "$p/product" 2>/dev/null))"
                break
            fi
            p="$(dirname "$p")"
        done
    done

    echo "=== dmesg tail ==="; dmesg 2>/dev/null | tail -30

    # --- READ-ONLY mount test of the first sd partition ---
    PART="$(ls /dev/sd*1 2>/dev/null | head -1)"
    [ -z "$PART" ] && PART="$(ls /dev/sda 2>/dev/null | head -1)"
    if [ -n "$PART" ]; then
        echo "=== READ-ONLY mount test: $PART ==="
        mkdir -p "$MP" 2>/dev/null
        if mount -o ro "$PART" "$MP" 2>&1; then
            echo "mounted OK (ro) at $MP"
            echo "--- df ---"; df -h "$MP" 2>/dev/null
            echo "--- top level ---"; ls -la "$MP" 2>/dev/null | head -30
            echo "--- /Samples present? ---"; [ -d "$MP/Samples" ] && echo "YES /Samples exists" || echo "NO /Samples"
            echo "--- /Samples subfolders ---"; ls -la "$MP/Samples" 2>/dev/null | head -60
            sync
            umount "$MP" 2>&1 && echo "unmounted cleanly" || echo "UMOUNT FAILED"
        else
            echo "mount (ro) FAILED for $PART — try -t vfat / -t exfat:"
            mount -t vfat -o ro "$PART" "$MP" 2>&1 && { echo "vfat ro OK"; ls "$MP" | head; umount "$MP"; } || echo "vfat ro failed"
            mount -t exfat -o ro "$PART" "$MP" 2>&1 && { echo "exfat ro OK"; ls "$MP" | head; umount "$MP"; } || echo "exfat ro failed"
        fi
    else
        echo "=== no /dev/sd* found ==="
        echo "M8 not in USB_DRIVE mode, not plugged, or mass-storage not enumerated."
    fi
    echo "=== done ==="
} > "$OUT" 2>&1
