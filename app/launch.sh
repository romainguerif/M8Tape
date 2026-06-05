#!/bin/sh
# Pak entry point. NextUI runs this; it execs the compiled app and PASSES THE PAK
# DIR as argv[1] so the app finds its res/ (fonts) and writes its logs there no
# matter what the current directory is. (Dropping that arg makes the app look for
# ./res relative to the wrong cwd -> fonts missing -> crash at first draw -> black
# screen. Learned the hard way.)

PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PLATFORM="${PLATFORM:-tg5040}"

# bundled libmsettings.so lives in the pak
export LD_LIBRARY_PATH="$PAK_DIR/lib:$LD_LIBRARY_PATH"

# FAT32/exFAT lose the exec bit -> (re)set it each launch
chmod +x "$PAK_DIR/m8tape-$PLATFORM" 2>/dev/null

LOG="$PAK_DIR/m8tape.log"
cd "$PAK_DIR"
"$PAK_DIR/m8tape-$PLATFORM" "$PAK_DIR" >"$LOG" 2>&1
