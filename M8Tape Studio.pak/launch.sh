#!/bin/sh
# M8Tape Studio - graphical sampling utility (SDL2). Pak entry point.
# Touches no USB storage / mount — only audio diagnostics + run the app.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

# audio diagnostics (safe: /proc/asound + arecord, no block-device I/O)
sleep 2
{
    echo "=== date ==="; date 2>/dev/null
    echo "=== arch / libc ==="; uname -a 2>/dev/null
    echo "=== arecord -l ==="; arecord -l 2>&1
    echo "=== /proc/asound/cards ==="; cat /proc/asound/cards 2>&1
    for s in /proc/asound/card*/stream0; do
        [ -f "$s" ] || continue
        echo "=== $s ==="; cat "$s" 2>&1
    done
} > "$PAK_DIR/diag.txt" 2>&1

export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"
"$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR" > "$PAK_DIR/m8tape.log" 2>&1
echo "exit code: $?" >> "$PAK_DIR/m8tape.log"
