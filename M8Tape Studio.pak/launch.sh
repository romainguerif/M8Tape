#!/bin/sh
# M8Tape Studio - minimal entry point. Does NOTHING with storage/USB/mount,
# so the M8 being in USB disk mode can never affect launch.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

# marker so we can confirm over HTTP which launch.sh actually ran
echo "LAUNCH_MINIMAL $(date 2>/dev/null)" > "$PAK_DIR/which_launch.txt" 2>/dev/null

chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

exec "$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR"
