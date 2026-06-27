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

At runtime the binary auto-discovers `third_party/MoltenVK/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json`
and sets `VK_ICD_FILENAMES` for the Vulkan loader. If you'd rather use
a system-installed MoltenVK (e.g. from the LunarG Vulkan SDK), set
`VK_ICD_FILENAMES` yourself before running and the binary respects it.

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
```

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
- **macOS Vulkan.** You need either the LunarG Vulkan SDK installed
  (provides the loader) or `brew install molten-vk` + setting
  `VK_ICD_FILENAMES`. Without one of these, `pl_vk_inst_create` fails.
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
| `src/probe.c`       | luminance probe: YUV → linear nits, source-side ground truth |
| `RENDERING.md`      | **Architecture doc — read first if changing rendering** |
| `CMakeLists.txt`    | pkg-config find + link |
