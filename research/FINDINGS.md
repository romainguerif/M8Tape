# M8Tape → Sampling utility — Research findings (2026-06-04)

Pre-coding research for evolving M8Tape into a full sampling utility for the
TrimUI Brick. Screenshots of the M8 sampler/editor are in `m8-screenshots/`.

## 1. UI — Nothing OS aesthetic (buildable as SDL2 app)
- Palette: pure black `#000000` bg, white `#FFFFFF` text, grey ramp
  (`#1E1E1E`/`#3A3A3A`/`#848484`/`#A7A7A7`/`#C5C5C5`), single accent **red
  `#D71921`** used on ≤2 elements/screen (record dot, clip, active selection).
  (Hex values are brand-faithful, not officially published. Ignore the purple
  "Nothing" palette on SchemeColor — misattribution.)
- Fonts: real Ndot / NType82 are **proprietary, do NOT ship**. Free OFL
  replacements: **DanielHartUK/Dot-Matrix-Typeface** (numerals/headlines),
  **Space Mono / Space Grotesk** (labels). Uppercase + generous tracking.
- Dot-matrix motif is the identity: render screen as a virtual LED grid (e.g.
  16px cells → 64×48), dots two-state ON(white/red)/OFF(dark grey). Route
  numbers, level meters, icons, and **waveforms** through one dot-grid renderer
  (waveform = quantize amplitude to vertical dot runs; playhead = lit column).
- Flat, hard edges, thin outlines, huge negative space. Motion = fast linear
  fades + dot ripples (~150–250ms), no bounce.

## 2. Platform / build (critical constraint)
- TrimUI Brick: 1024×768 4:3 IPS, Allwinner A133P, 4×Cortex-A53 aarch64, 1GB RAM,
  platform `tg5040`. Controls: D-pad, ABXY, L1/R1/L2/R2, Select/Start/Menu.
- **Device is glibc 2.33, NOT musl.** Our `zig cc aarch64-linux-musl` works for
  standalone C (m8split) but CANNOT dynamically link the device's SDL2.
- SDL2 2.30.8 + SDL2_ttf + SDL2_image ship on device. Custom paks render via
  MinUI `GFX_*`/`PAD_*` helpers over SDL2 (RGB565 framebuffer).
- **Proven path for a graphical app = Docker `union-tg5040-toolchain`**
  (shauninman) — glibc aarch64 + SDL2/ttf/image + MinUI libs. Template:
  `josegonzalez/minui-sample-sdl2-app-pak`. Needs Docker on the Mac.
- A pak's `launch.sh` can be a compiled binary; bundle SDL_ttf/SDL_image `.so`
  with `LD_LIBRARY_PATH` to be safe.
- Verify on-device: `arch` (aarch64), `ldd --version` (glibc ~2.33),
  `ls /usr/lib | grep -i SDL`.

## 3. Devices & capture (auto-detect feasible)
- **M8**: 24ch S24_3LE 44.1k (known working, current pipeline).
- **Model:Samples**: class-compliant USB audio 2.0, **stereo out 48k**, no driver
  → `arecord` works. High confidence. Same USB-host mechanism as M8.
- **OM System LS-P5**: YES, it's a USB Audio Class device ("Composite USB
  Microphone Mode") — must set Composite/USB-audio mode on the recorder (not its
  default mass-storage). Exact USB rate/depth/channels undocumented → confirm with
  `arecord --dump-hw-params`. Tested working as interface in Reaper on Mac.
- **Auto-detection**: ALSA C API
  `snd_pcm_hw_params_get_channels_max()` per capture card → `>=24` = M8
  multitrack, else stereo. Enumerate with `snd_card_next()`. Display detected
  source. CLI fallback: `arecord -l`, `arecord --dump-hw-params -D hw:X,0`.

## 4. Sample editing — M8-inspired feature set
M8 sampler params: SLICE, PLAY (FWD/REV/loops/ping-pong/OSC/REPITCH-BPM), START,
LOOP ST., LEN, DETUNE, DEGRADE + filter/amp/mixer. Sample = mono/stereo PCM WAV,
streamed from SD (pitch ceiling tied to bit depth/channels).

**Sample Editor PROCESS ops (our edit/convert feature target):**
CROP (trim to range), DELETE, DUPLICATE, NORMALIZE, SILENCE, REVERSE, INVERT,
FADE IN/OUT, XFADE LOOP (click-free loops), MONO: MIX/LEFT/RIGHT, DOWNSAMPLE
(halve rate), 16-BIT / 8-BIT conversion, SLICE: AUTO(transient)/SILENC/[0–128].
Waveform with SELECT range + LOOP region markers, zoom on fine edit, up to 128
slice cue markers stored in the WAV.
→ **Conversion = the MONO/DOWNSAMPLE/16-8BIT ops, done inside edit mode.**

## 5. Silence auto-trim (toggleable)
Trim leading/trailing silence below a threshold. Implement in our own C (we own
the WAV reader from m8split) — no dependency on sox/ffmpeg on device. Toggle on/off.

## Other sample tools found (browser/desktop, not on-device)
underpass (sample directly onto M:S, Chrome WebMIDI), elk-herd (+Drive file
manager), Elektroid (desktop sample manager, supports M:S) — relevant to the
separate "send TO Model:Samples" idea, not this capture-focused phase.

## Key sources
Nothing: androidauthority OS3 hands-on; DanielHartUK Dot-Matrix-Typeface (OFL).
Platform: LoveRetro/NextUI PAKS.md; shauninman/union-tg5040-toolchain;
josegonzalez/minui-sample-sdl2-app-pak; trimui/firmware_smartpro (glibc 2.33,
SDL2 2.30.8). Devices: explore.omsystem.com/us/en/ls-p5;
soundonsound.com/reviews/om-system-ls-p5; elektron.se/product/modelsamples.
M8: official Operation Manual v6.0.0.
