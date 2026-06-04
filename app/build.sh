#!/bin/sh
# build.sh - cross-compile the M8Tape SDL2 app for the TrimUI Brick (tg5040)
# using the union-tg5040 Docker toolchain. Run from a Mac/PC with Docker.
#
#   ./build.sh         # build app/m8tape-tg5040
#   ./build.sh clean   # remove the built binary
#
# The toolchain image is built once from toolchain/union-tg5040-toolchain
# (see app/README.md). MinUI is cloned into app/minui on first build.

set -e
cd "$(dirname "$0")"

IMAGE=tg5040-toolchain

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Docker image '$IMAGE' missing. Build it once:"
    echo "  cd toolchain/union-tg5040-toolchain && docker build --platform linux/amd64 -t $IMAGE ."
    exit 1
fi

if [ "$1" = "clean" ]; then
    rm -f m8tape-tg5040
    echo "cleaned."
    exit 0
fi

# make minui first so makefile.env (which sets SDL=SDL2) exists before the
# real build parses it; otherwise it would fall back to SDL 1.2 headers.
docker run --rm --platform linux/amd64 -v "$PWD":/root/workspace "$IMAGE" \
    bash -lc 'source /root/setup-env.sh && cd /root/workspace && \
              make minui >/dev/null 2>&1 || true; \
              make'

echo
echo "built: app/m8tape-tg5040"
file m8tape-tg5040 2>/dev/null || true
