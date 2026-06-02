#!/bin/sh
# build.sh - compile m8split pour la TrimUI Brick (aarch64) avec zig.
# Produit un binaire statique (musl), strippé, et le dépose dans le pak.
#
# Prérequis : zig (https://ziglang.org). Sur macOS : brew install zig
#
# Usage :
#   ./build.sh            # binaire aarch64 -> ../M8Tape.pak/bin/tg5040/m8split
#   ./build.sh native     # binaire pour cette machine (test local)

set -e
cd "$(dirname "$0")"

command -v zig >/dev/null 2>&1 || { echo "zig introuvable. macOS : brew install zig"; exit 1; }

if [ "$1" = "native" ]; then
    zig cc -O2 -std=c11 -Wall -Wextra -o m8split-native m8split.c
    echo "m8split-native (cette machine) compilé."
    exit 0
fi

# Cible TrimUI Brick : aarch64, statique (musl), strippé (-Wl,-s).
zig cc -O2 -std=c11 -Wall -Wextra -target aarch64-linux-musl -Wl,-s -o m8split m8split.c

DEST="../M8Tape.pak/bin/tg5040"
mkdir -p "$DEST"
cp m8split "$DEST/m8split"
chmod +x "$DEST/m8split"
echo "m8split (aarch64, statique) -> $DEST/m8split"
