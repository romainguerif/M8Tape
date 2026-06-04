#!/bin/sh
# Pak entry point. NextUI runs this; it just execs the compiled app and logs
# stdout/stderr next to the pak for debugging.

PAK_DIR="$(dirname "$0")"
PLATFORM="${PLATFORM:-tg5040}"

# make sure the binary is executable (FAT32/exFAT lose the bit)
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

LOG="$PAK_DIR/m8tape.log"
"$PAK_DIR/m8tape-$PLATFORM" >"$LOG" 2>&1
