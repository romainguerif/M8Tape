#!/bin/sh
# M8Tape Studio - graphical sampling utility (SDL2). Pak entry point.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

# FAT32/exFAT cards don't store the exec bit; re-set it each launch.
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

# --- diagnostics (so we can validate detection remotely over HTTP) ----------
# small wait so a freshly-plugged USB audio device has time to enumerate
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
    for i in /proc/asound/card*/id; do
        [ -f "$i" ] || continue
        echo "=== $i ==="; cat "$i" 2>&1
    done
    echo "=== SDL libs present ==="; ls -1 /usr/lib/ 2>/dev/null | grep -i sdl
} > "$DIAG" 2>&1

# bundle our libmsettings.so (system SDL2/ttf/image are provided by NextUI).
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

"$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR" >"$PAK_DIR/m8tape.log" 2>&1
echo "exit code: $?" >> "$PAK_DIR/m8tape.log"
