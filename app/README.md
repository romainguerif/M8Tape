# M8Tape Studio — app source

Graphical sampling utility for the TrimUI Brick (NextUI / `tg5040`), built as a
compiled SDL2 app using the MinUI `GFX_*`/`PAD_*` framework. Nothing-OS-style UI.

## Build

Requires Docker. The cross-toolchain image is built once:

```sh
cd ../toolchain/union-tg5040-toolchain
docker build --platform linux/amd64 -t tg5040-toolchain .
```

Then, from this directory:

```sh
./build.sh          # produces app/m8tape-tg5040 (aarch64 glibc, dynamic SDL2)
./build.sh clean
```

`build.sh` runs the toolchain container, clones MinUI into `app/minui` on first
run (for the GFX/PAD libs + platform layer), builds `libmsettings.so`, and links
against the device's SDL2 / SDL2_ttf / SDL2_image.

## Packaging

The built binary is copied into `../M8Tape Studio.pak/` along with
`lib/libmsettings.so`; `launch.sh` sets `LD_LIBRARY_PATH` and execs it. Copy that
`.pak` folder to `Tools/tg5040/` on the SD card. (SDL2/ttf/image are provided by
NextUI; only `libmsettings.so` is bundled.)

## Milestones

- **M1 (done):** boots, Nothing-style screen, auto-detects the USB capture
  source from `/proc/asound` (channels / rate / format), classifies M8 vs stereo.
- M2 capture · M3 browser+playback · M4 waveform editor · M5 conversion +
  silence-trim · M6 Nothing visual polish (dot-matrix font, dot rendering).

## Notes

- Device is **glibc**, so this binary is built with the glibc aarch64 toolchain
  (not the `zig`/musl flow used for `m8split`). The two coexist.
- `minui/` and `platform/` are build artifacts (git-ignored).
