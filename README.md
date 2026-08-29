# hdrplay

A ~900-line CLI HDR video player built for **insight, not features**.
Companion to `vca.py`: where `vca` answers "is this file really HDR?",
`hdrplay` answers "did the HDR pipeline actually reach the display?"

## The pipeline, in one diagram

```
file                                      [DEC]   demux + decode
 │
 ▼
AVFrame (10-bit P010 / YUV420P10 / …)    [META]  HDR10 side data extracted
 │
 ▼
pl_map_avframe_ex ── pl_frame             [REND]  uploaded to GPU
 │
 ▼
pl_renderer (libplacebo)                  [REND]  tone-map, gamut, dither
 │   ├─ source colorspace from AVFrame
 │   ├─ target colorspace from swapchain
 │   └─ dynamic peak luma from live EDR headroom
 ▼
Vulkan swapchain image                    [SWAP]  surface colorspace
 │                                                 (BT2020_PQ if display HDR)
 ▼
SDL3 / OS compositor                      [HDR]   signals HDR to panel
 │                                                 (CAMetalLayer / Wayland
 │                                                  color-mgmt / DXGI HDR)
 ▼
display
```

## Build

### macOS

```bash
# Dependencies (Homebrew):
brew install ffmpeg libplacebo sdl3 cmake pkg-config vulkan-loader vulkan-headers

# MoltenVK (the Vulkan→Metal driver). No brew needed — the build
# system looks for it bundled under ./third_party/. Grab the release:
mkdir -p third_party && cd third_party && \
  curl -L -o MoltenVK-macos.tar https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.1/MoltenVK-macos.tar && \
  tar xf MoltenVK-macos.tar && cd ..

cmake -B build -S .
cmake --build build
./build/hdrplay path/to/hdr10.mp4 -v
```

To put it on your `PATH`, use the install target rather than copying
the binary by hand — it drops the MoltenVK ICD and dylib next to the
executable so the installed copy is self-contained:

```bash
cmake --install build --prefix ~     # → ~/bin/hdrplay + MoltenVK_icd.json + libMoltenVK.dylib
```

At runtime the binary locates a MoltenVK ICD manifest and points the
Vulkan loader at it (setting `VK_DRIVER_FILES` / `VK_ICD_FILENAMES`).
It tries, in order:

1. `VK_DRIVER_FILES` or `VK_ICD_FILENAMES` in the environment — a one-off override
2. `vulkan_icd` in `~/.config/hdrplay/config` — a durable one (see **Config** below)
3. `MoltenVK_icd.json` next to the executable (what `cmake --install` sets up)
4. `third_party/MoltenVK/…` relative to the executable — covers `./build/hdrplay`
5. `third_party/MoltenVK/…` in the source tree this binary was **built** from,
   baked in at compile time by CMake — so a hand-copied binary still works
6. `$VULKAN_SDK/share/vulkan/icd.d/` (LunarG SDK)
7. Homebrew's `molten-vk` keg

If nothing is found, the `[GPU]` log prints every path it tried.

### Linux (Ubuntu 24.04+)

```bash
sudo apt install build-essential cmake pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libplacebo-dev libsdl3-dev libvulkan-dev vulkan-tools mesa-vulkan-drivers
cmake -B build -S .
cmake --build build
./build/hdrplay path/to/hdr10.mp4 -v
```

If apt's SDL3 is missing (< 24.04), build from source:
```bash
git clone --depth 1 https://github.com/libsdl-org/SDL && \
  cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release && \
  cmake --build SDL/build && sudo cmake --install SDL/build
```

## Use

```bash
hdrplay video.mp4                  # plain run
hdrplay video.mp4 -v               # verbose per-frame logging
hdrplay video.mp4 -f               # start fullscreen (recommended for true HDR)

# F toggles fullscreen, Q/Esc quits.
# I toggles the status HUD, A the accumulated-statistics panel.
# . and , step one frame forward / back.
```

### Comparing two files

```bash
hdrplay a.mov b.mov        # synchronized, side by side, one window
```

A single **PTS master clock** drives both files: each shows the frame in
effect at that instant. Two files at different frame rates therefore
land on different frame *indices* at the same *moment*, which is what
synchronized has to mean when the rates differ — frame-index lockstep
would drift them apart linearly. A shorter file holds its last frame
instead of going black.

With two files the split becomes the **layout**, so `H`/`S` apply to
both panes and you compare A-vs-B under HDR, then A-vs-B under SDR.
Varying content and treatment at once would leave any difference you see
with two possible causes. Press `1` or `2` to solo a file, which drops
back to exactly single-file behaviour — including the HDR-vs-SDR split —
and `0` to return.

| Key | |
|---|---|
| `.` `,` | step one frame forward / back (pauses) |
| `0` `1` `2` | compare A\|B / solo A / solo B |
| `X` | swap sides |
| `Z` | toggle 1:1 zoom |
| `+` `-` | zoom steps |
| drag, `shift`+arrows | pan, locked across panes |
| `P` `O` | split mode / cycle LR, TB, diagonal wipe |

**Stepping backward** is the awkward direction — video decodes one way,
so frame N−1 normally means seeking to the preceding keyframe and
decoding forward again. hdrplay retains the last few frames per file so
short back-steps are instant, falling back to seek beyond that.
`--step-buffer N` sets the depth (default 8, `0` disables). Retained
frames cost ~25 MB each at 4K 10-bit, ~6 MB at 1080p.

**Zoom matters more than it sounds.** A half-pane is ~960 px wide, so in
fit mode both files are downscaled and you can only see gross
differences — grade, banding, blown highlights. `Z` gives 1:1 source
pixels, which is where compression artifacts actually become visible.
Pan is locked across panes, so you are always looking at the same region
of both.

Files of different resolutions are fine. Zoom is expressed against the
larger of the two, so both panes always cover the same region of the
scene rather than the same pixel count — otherwise at 1:1 a 1080p pane
would show four times the area of a 4K one.

### Content analysis

Per-frame statistics answer "is *this frame* HDR?". Accumulated ones
answer the question you actually have: **is this file worth using as an
HDR test clip?**

Press `A` during playback for a live session panel (peaks that latched
earlier in the clip, percentiles over everything seen so far, and how
much of the file that covers). `shift-A` resets it. The accumulator
dedupes by PTS high-water mark, so seeking and `--loop` cost nothing and
cannot double-count. A summary prints on exit either way.

For a verdict on the whole file, scan it headlessly — no window, no GPU,
works over SSH:

```bash
hdrplay --analyze clip.mov
```

```
content checks  clip.mov
  PASS  coverage                          72 frames, 100% of duration
  INFO  luminance reference               PQ absolute
  PASS  content exceeds SDR range         p99.9 = 3520N
  FAIL  MaxCLL vs declared                declares 400N but pixels reach 10000N
                                          under-declared: tone mappers trust this value
  PASS  dynamic range                     14.4 stops (p99.9/p1)
  INFO  spread                            4.24 spatial / 0.01 temporal stops

summary: 1 FAIL, 0 WARN
```

Exit code is the FAIL count, so batch triage works directly:

```bash
for f in *.mov; do hdrplay --analyze "$f" || echo "$f suspect"; done
```

Exit codes **>= 64** are tool errors (unreadable file, unsupported pixel
format), not content verdicts — otherwise a missing file is
indistinguishable from "1 FAIL".

Three things worth understanding about the numbers:

- **Measurements are one-sided lower bounds.** Sampling stride, luma vs
  the spec's `max(R,G,B)`, and Jensen's inequality on a convex EOTF all
  push the estimate *down*. So `measured > declared` is real evidence of
  under-declaration and gets a FAIL, while `measured <= declared` proves
  nothing and is reported as INFO — never PASS. `--analyze` defaults to
  `--stride 1` (every pixel) precisely so the FAIL side is sound.
- **HLG numbers rest on an assumption.** HLG carries no absolute
  luminance; converting scene light to display light needs a nominal
  peak `L_W`. hdrplay takes the file's mastering-display max, else the
  BT.2100 reference of 1000 nits, and always says which. Override with
  `--hlg-peak`.
- **SDR gets no absolute figures at all.** A measured MaxCLL for an SDR
  file cannot exceed 100 nits by construction, so those checks are
  suppressed rather than printed with a caveat. Ratio statistics
  (dynamic range, spread) are still valid and still shown.

`--json` writes a machine-readable summary to stdout (checks stay on
stderr, so `| jq` works). `--stats-file out.ndjson` writes a per-frame
series plus session histograms for plotting in `vca.py`.

### Config

Persistent settings live at `~/.config/hdrplay/config`, or
`$XDG_CONFIG_HOME/hdrplay/config` if that variable is set. Nothing is
required — the file is optional and hdrplay runs fine without it.

Format is `key = value`, one per line. Blank lines and `#` comments are
ignored, surrounding whitespace is trimmed, and a leading `~/` in a
value expands to `$HOME`.

```ini
# ~/.config/hdrplay/config

# Where to find the MoltenVK ICD manifest (macOS). Overrides
# auto-discovery; lower priority than VK_DRIVER_FILES in the env.
vulkan_icd = ~/code/hdrplay/third_party/MoltenVK/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json
```

Unrecognized keys are ignored, so this is the place to add future
playback settings.

### Reading the logs

```bash
hdrplay video.mp4 2>&1 | grep '^\[DEC\]'    # what was decoded
hdrplay video.mp4 2>&1 | grep '^\[META\]'   # what the file claims
hdrplay video.mp4 2>&1 | grep '^\[HDR\]'    # display HDR state changes
hdrplay video.mp4 2>&1 | grep '^\[SWAP\]'   # swapchain colorspace
hdrplay video.mp4 2>&1 | grep '^\[REND\]'   # libplacebo's per-frame work
```

### Was HDR actually delivered?

Three signals, in order of trustworthiness:

1. **`[HDR] display state: hdr=ON, headroom=4.20x`** — SDL says the OS
   compositor created the window's swapchain in HDR mode. If this is
   `hdr=off`, nothing else matters: the OS is asking for SDR.
2. **`[SWAP] swapchain ready, HDR signaling ACTIVE`** — Vulkan accepted
   an HDR surface format. If this is `off` but display state was `ON`,
   your Vulkan/MoltenVK build is missing `VK_KHR_swapchain_colorspace`.
3. **The panel's own info / OSD button** — TVs and HDR monitors will
   display `HDR10` / `HLG` / `Dolby Vision` when the signal lands.
   This is ground truth; software can lie, panels rarely do.

## Caveats / known fixups

This scaffold is structurally complete but the author hasn't built it
on your machine. Expect small fixups on first compile:

- **SDL3 property names.** `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN` and
  friends were renamed during SDL 3.0 → 3.2. If clang errors on those
  symbols, grep `SDL_video.h` in your install for `HDR` to find the
  current name and substitute in `renderer.c:renderer_update_display_state`.
- **libplacebo 6 vs 7.** `pl_map_avframe_ex` and `pl_avframe_params`
  exist in libplacebo ≥ 6.x. On 7.x they're identical in shape.
  If you're on an older release, the equivalent is `pl_upload_avframe`.
- **macOS Vulkan.** Two separate pieces, and it's easy to have one
  without the other. The **loader** comes from `brew install
  vulkan-loader` or the LunarG SDK; the **driver** (MoltenVK) comes
  from `third_party/`, `brew install molten-vk`, or the SDK. Loader
  but no driver is the common case, and SDL reports it misleadingly:
  `SDL_CreateWindowWithProperties: Installed Vulkan Portability library
  doesn't implement the VK_KHR_surface extension`. That means "no ICD",
  not "bad SDL". Check the `[GPU] MoltenVK ICD found:` log line — if
  it's missing, see the discovery order under **Build → macOS**.
- **Linux Wayland HDR.** Only mainline KDE and recent Mutter implement
  the `color-management-v1` protocol. On other compositors HDR signaling
  silently degrades to SDR; the `[HDR]` log will show `hdr=off` and
  libplacebo will tone-map for you. Still useful — just not "real" HDR.

## What this is not

- Not an A/V sync'd media player. No audio, no seek, no subtitles.
- Not production code. No error recovery from mid-stream format changes.
- Not a benchmark — it sleeps for nothing; FPS is whatever the swapchain
  pacing allows.

For all of those, use mpv. This is a teaching tool.

## Architecture & gotchas

**Read [`RENDERING.md`](./RENDERING.md) before changing anything in
`renderer.c`.** It explains why the SDR path looks weirdly indirect (it
has to be — there's a non-obvious asymmetry in how libplacebo handles
`target.color.hdr.max_luma` between swapchain targets and texture
targets), documents every libplacebo gotcha we hit, and answers the
recurring "why is SDR brighter than I expect / dimmer than ffplay"
questions.

## Files

| File | What it does |
|---|---|
| `src/main.c`        | arg parse, event loop, frame pump, key handling |
| `src/decoder.c`     | ffmpeg demux + decode, HDR side-data extraction |
| `src/renderer.c`    | SDL3 + Vulkan + libplacebo init, per-frame render, SDR-via-overlay composition |
| `src/hud.c`         | embedded bitmap font, on-screen status panel + split badges |
| `src/diagnose.c`    | `--diagnose` HDR sanity checks (per-display PASS/WARN/FAIL) |
| `src/brightness.c`  | `--set-brightness` via IOKit / `brightness` CLI / m1ddc |
| `src/probe.c`       | luminance probe + per-frame histograms: YUV → linear nits, source-side ground truth |
| `src/stats.c`       | session accumulation: PTS-deduped histograms, percentiles, spread decomposition |
| `src/layout.c`      | pure render planning: passes, crops, masks, overlay routing (GPU-free, so it can be tested) |
| `src/source.c`      | one input: decode, frame ring for step-back, clock following |
| `src/analyze.c`     | `--analyze` headless whole-file scan, `--json`, `--stats-file` |
| `src/checks.c`      | shared PASS/WARN/FAIL reporting for `--diagnose` and `--analyze` |
| `tests/`            | probe, accumulator, layout and source tests (`ctest --test-dir build`) |
| `RENDERING.md`      | **Architecture doc — read first if changing rendering** |
| `CMakeLists.txt`    | pkg-config find + link |
