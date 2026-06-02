# Building `m8split`

`m8split` splits an interleaved multichannel PCM WAV into stereo pairs. For
M8Tape it turns a 24-channel take into 12 stereo files (`01.wav` … `12.wav`),
pair *N* being channels *2N-1* and *2N*. Single read pass; no dependencies.

It is plain C ([`m8split.c`](m8split.c)) and is cross-compiled with
[zig](https://ziglang.org) as the C cross-compiler, producing a **static**
aarch64 binary that runs on the TrimUI Brick without any runtime libraries.

## Prerequisites

- `zig` — on macOS: `brew install zig`

## Build

```sh
./build.sh            # aarch64 static binary -> ../M8Tape.pak/bin/tg5040/m8split
./build.sh native     # binary for the host machine (for local testing)
```

Or directly:

```sh
zig cc -O2 -std=c11 -target aarch64-linux-musl -Wl,-s -o m8split m8split.c
```

Then copy `m8split` into `M8Tape.pak/bin/tg5040/` and `chmod +x` it.

## Usage

```sh
m8split input_Nch.wav output_dir
```

Works for any even channel count and PCM depth (8/16/24/32-bit integer).
Exit codes: `0` ok, `1` usage, `2` I/O error, `3` invalid/unsupported WAV.
