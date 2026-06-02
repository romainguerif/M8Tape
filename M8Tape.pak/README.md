# M8Tape

Multichannel recorder for the [Dirtywave M8](https://dirtywave.com/) on the
TrimUI Brick (NextUI / MinUI).

M8Tape captures the M8's 24-channel USB audio output and splits each take into
12 stereo files, ready to mix. Capture format: 24 channels, S24_3LE (packed
24-bit), 44100 Hz.

## Output

One timestamped folder per take:

```
M8Tape.pak/rec_YYYYMMDD_HHMMSS/
  01.wav   (channels 1+2)
  02.wav   (channels 3+4)
  ...
  12.wav   (channels 23+24)
```

Pair *N* contains channels *2N-1* and *2N*. The raw 24-channel WAV is removed
after a successful split.

## Requirements

- TrimUI Brick running NextUI (or a `tg5040`-compatible MinUI).
- Dirtywave M8 with USB audio output enabled.
- USB-C data cable, connected to the Brick's top USB-C port.
- A fast microSD card (V30 or better) recommended for direct recording.

## Installation

1. Copy the `M8Tape.pak` folder into `Tools/tg5040/` on the SD card.
2. Place the two `tg5040` binaries in `M8Tape.pak/bin/tg5040/` (see
   [`bin/tg5040/PLACE_BINARIES_HERE.txt`](M8Tape.pak/bin/tg5040/PLACE_BINARIES_HERE.txt)):

   | Binary | Purpose | Source |
   |---|---|---|
   | `minui-list` | native menu | [josegonzalez/minui-list releases](https://github.com/josegonzalez/minui-list/releases/latest), `tg5040` asset |
   | `m8split` | 24ch → 12 stereo split | build from source — see [`m8split-src/BUILD.md`](m8split-src/BUILD.md) |

   These binaries are not committed to the repository. `m8split` is built from
   the included C source with a single `zig` command (see below).
3. Run `chmod +x` on `launch.sh` and both binaries.

## Usage

1. Connect the M8 (top USB-C port, data cable, USB audio enabled).
2. Open **Tools > M8Tape**.
3. **Start** begins the take; the menu then shows **Stop**, the start time, and
   the current folder.
4. **Stop** ends the recording: `arecord` finalizes the WAV header, then the
   split produces the twelve stereo files.

The recording may be left running when leaving the menu, and stopped on a later
visit.

## How it works

- **USB capture** via `arecord` at the fixed 24ch / S24_3LE / 44.1 kHz format.
- **USB stability**: `snd_usb_audio.nrpacks=1`, USB autosuspend disabled, and
  device power control forced on for the duration of the take.
- **Scheduling**: elevated process priority (`nice -n -19`) and large
  `arecord` buffers.
- **Split**: via `m8split` if present, otherwise `sox`; if neither is
  available, the raw 24-channel WAV is kept intact. `m8split` is a small static
  binary built from [`m8split-src/`](m8split-src/) — see its `BUILD.md`.

### Recording modes

- **Direct (default)**: writes straight to the card, no duration limit.
- **RAM (fallback for slow cards)**: create an empty file named `use_ram` in
  `M8Tape.pak/` to record into memory and copy to the card on stop. This
  removes glitches on slow cards but limits take length to available RAM
  (~3 min on the Brick). Delete `use_ram` to return to direct mode.

## Desktop splitting (`split_m8.sh`)

If a take comes back from the device as a raw 24-channel WAV (no splitter was
available on the Brick), split it on a computer with
[`split_m8.sh`](split_m8.sh). It requires no compilation and uses `ffmpeg` or
`sox`, whichever is installed.

```sh
chmod +x split_m8.sh
./split_m8.sh take_24ch.wav [output_dir]
```

Output defaults to a `<name>_stems/` folder next to the input, containing
`01.wav` … `12.wav` (same pairing as on-device).

## Fallbacks and troubleshooting

| Situation | Behavior |
|---|---|
| No `minui-list` | Toggle mode (first launch starts, next stops); no menu. |
| No `m8split` | `sox` is used if present; otherwise the raw 24ch WAV is kept. |
| `arecord` missing | Status written to `state/status.txt`; nothing is recorded. |
| Issue during a take | See `state/status.txt` and `state/arecord.log`. |

## Limitations

- **FAT32**: the raw take is capped at 4 GB (~21 min) before split. **exFAT**
  has no limit.
- **RAM mode**: take length bounded by available RAM (~3 min on the Brick).

## Status

The full pipeline (per-take folder, capture, 12-stereo split, raw cleanup) has
been simulated and verified off-device, and the `m8split` logic tested on real
24-channel WAVs. Real `arecord` capture and `minui-list` rendering remain to be
confirmed on the Brick.

## Repository layout

```
M8Tape.pak/
  launch.sh   capture, USB tuning, split, fallbacks
  pak.json    NextUI label
  README.md   on-device copy of this document
  bin/tg5040/ place minui-list and m8split here (not committed)
m8split-src/  m8split C source, build script and BUILD.md
split_m8.sh   desktop splitter (ffmpeg/sox)
```
