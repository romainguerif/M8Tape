#!/bin/sh
# M8Tape Studio - graphical sampling utility (SDL2). Pak entry point.
PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

# FAT32/exFAT cards don't store the exec bit; re-set it each launch.
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

# bundle our libmsettings.so (system SDL2/ttf/image are provided by NextUI).
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

"$PAK_DIR/m8tape-$PLATFORM" >"$PAK_DIR/m8tape.log" 2>&1
