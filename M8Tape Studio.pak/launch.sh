#!/bin/sh
# M8Tape Studio - diagnostic entry point. Touches NO storage/USB/mount.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

echo "LAUNCH_STAGE $(date 2>/dev/null)" > "$PAK_DIR/which_launch.txt" 2>/dev/null
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

"$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR" > "$PAK_DIR/m8tape.log" 2>&1
echo "exit code: $?" >> "$PAK_DIR/m8tape.log"
