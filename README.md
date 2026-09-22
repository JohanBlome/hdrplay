# hdrplay

A command-line HDR video player for inspecting source signals and verifying
that HDR reaches the display. It combines color-managed playback with
comparison views, source-level diagnostics, an RGB waveform and headless
content analysis.

## The pipeline, in one diagram

```
file                                      [DEC]   demux + hardware decode
 │
 ▼
AVFrame (10-bit P010 / YUV420P10 / …)    [META]  HDR10 + H.274 AMVE side data extracted
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

On macOS, supported formats are decoded by VideoToolbox. On Linux,
hdrplay tries VAAPI (Intel/AMD) and then CUDA (NVIDIA, when enabled in
FFmpeg). The hardware surface is downloaded as NV12/P010 for the pixel
probes and session statistics; colour conversion and presentation remain
on the GPU. Unsupported profiles, codecs and systems automatically use
FFmpeg's multithreaded software decoder.

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

To put it on your `PATH`, use the install target rather than copying the
binary by hand. The install also places the MoltenVK ICD and dylib beside the
executable:

```bash
# User-local installation:
cmake --install build --prefix "$HOME/.local"

# Or system-wide under /usr/local (quote the resolved Homebrew path because
# sudo may not inherit your shell's PATH):
sudo "$(command -v cmake)" --install build --prefix /usr/local
```

For the user-local option, ensure `$HOME/.local/bin` is on `PATH`.

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

### Fedora Linux 43+

Fedora's own FFmpeg build (`ffmpeg-free`) compiles hdrplay, but ships no
H.264 or HEVC decoder at all — only AV1 and VP9. Most HDR10 content is
HEVC, so a stock-Fedora build will fail to open typical test files. RPM
Fusion Free supplies the missing decoders; there are two ways to get them.

**Recommended — keep Fedora's FFmpeg, add the codec library:**

```bash
sudo dnf install \
  "https://download1.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
sudo dnf install \
  gcc cmake pkgconf-pkg-config ffmpeg-free-devel libavcodec-freeworld \
  libplacebo-devel SDL3-devel \
  vulkan-loader-devel vulkan-headers mesa-vulkan-drivers
```

`libavcodec-freeworld` installs into `/usr/lib64/ffmpeg/`, which
`/etc/ld.so.conf.d/ffmpeg-lib64.conf` places ahead of `/usr/lib64` — so it
transparently replaces `libavcodec` at load time. Build against the
`ffmpeg-free-devel` headers as normal; no `--allowerasing`, and no other
application on the system is affected.

**Alternative — replace FFmpeg wholesale:**

```bash
sudo dnf install --allowerasing \
  gcc cmake pkgconf-pkg-config ffmpeg ffmpeg-devel \
  libplacebo-devel SDL3-devel \
  vulkan-loader-devel vulkan-headers mesa-vulkan-drivers
```

This swaps `ffmpeg-free` for RPM Fusion's `ffmpeg` system-wide, affecting
every application that links FFmpeg.

Then, either way:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
cmake --build build --target check
./build/hdrplay path/to/video.mp4
```

`mesa-vulkan-drivers` supplies Vulkan for Intel and AMD GPUs; systems
using NVIDIA's proprietary driver already obtain Vulkan from that
driver instead.

### Ubuntu 24.04+

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
hdrplay video.mp4
hdrplay -f video.mp4                  # fullscreen HDR playback
hdrplay --waveform video.mp4          # start with the RGB waveform
hdrplay --gamut video.mp4             # start with the CIE gamut scope
hdrplay --vectorscope video.mp4       # start with the Cb/Cr vectorscope
hdrplay --histogram video.mp4         # start with the RGB histogram
hdrplay first.mov second.mov          # synchronized comparison
```

### Runtime controls

| Key | Action |
|---|---|
| `F` | toggle fullscreen |
| `H` / `S` | select HDR or SDR treatment |
| `P` | HDR/SDR split for one file; comparison for two files |
| `O` | cycle split orientation and comparison wipes |
| `C` | cycle color, Y, Cb, Cr, legal, clip and plateau views |
| `V` | cycle scopes: off, waveform, gamut, vectorscope, histogram |
| `G` | toggle vectorscope trace gain between 1x and 2x |
| `D` | toggle current/previous or A/B difference |
| `Space` | pause or resume |
| `.` / `,` | step one frame forward or backward |
| `←` / `→` | seek backward or forward 10 seconds |
| `Z`, `+`, `-` | fit/1:1 and zoom controls |
| drag or `Shift`+arrows | pan while zoomed |
| `M` | toggle the mouse luminance probe |
| `I` | toggle the status HUD |
| `A` / `Shift-A` | toggle or reset accumulated statistics |
| `T` | rotate the focused source 90° clockwise |
| `W` | resize the window for exact 1:1 display |
| `0`, `1`, `2` | compare both files or solo the first/second |
| `X` | swap comparison sides |
| `L` / `R` | toggle looping or restart |
| `Q` / `Esc` | quit |

### Plane inspection

Press `C` to cycle through normal color, Y, Cb, Cr, `LEGAL`, `CLIP` and
`PLATEAU`. Each selected
component is repeated into RGB and shown as grayscale, making quantization
steps in the color-difference planes much easier to see. Chroma is sampled
nearest-neighbor when enlarged so the viewer does not hide boundaries by
interpolating them. The selected plane applies to both files in comparison
mode and remains independent of HDR/SDR treatment. A persistent badge in the
top-right identifies the current view, even when the status HUD is hidden.
`LEGAL` marks decoded luma at/beyond nominal video-range white red and nominal
black blue (for example 940/64 in 10-bit limited range), which exposes range
mislabelling. `CLIP` is stricter: only the literal storage endpoints are red
and blue (1023/0 in 10-bit), regardless of nominal range. Everything else is
grayscale. Both tests inspect the delivered source before HLG, tone mapping
or display processing. Endpoint contact proves saturation of the delivered
code value, but cannot identify whether the camera, grade or encoder caused
it, or whether it was intentional. `PLATEAU` finds a different signature:
locally flat luma in the outer 10% of the usable range. High candidates are
red and low candidates blue. Local changes up to eight source code values are
treated as flat so that compression noise around an earlier clipping plateau
does not hide it. This is evidence, not proof: naturally uniform bright or
dark areas can also be marked.

Start directly in a component view with `--plane y`, `--plane cb` or
`--plane cr`; `--plane legal`, `--plane clip` and `--plane plateau` start the
false-color views, and `u`, `v` and `flat` are accepted aliases.

### Video scopes

Press `V` to cycle between no scope, a channel-overlaid RGB waveform, a CIE
1931 xy gamut plot, a digital vectorscope and an RGB histogram. `--waveform`,
`--gamut`, `--vectorscope` and `--histogram` select a scope at startup.

In the waveform, horizontal position follows the source image and vertical
position is the encoded R'G'B' signal level before transfer conversion, HLG
processing, tone mapping or display color management. Red, green and blue
traces are density-weighted and add to white where the channels coincide.

The grid marks nominal 0%, 25%, 50%, 75% and 100%, with -10% and 110% guard
bands retained above and below. Those guard bands make range excursions and
capture-processing overshoot visible instead of clipping them at the edge of
the graph. The scope occupies 40% of the framebuffer in each dimension, with
a 640x360-pixel minimum where space permits.

The gamut plot converts sampled source pixels to CIE xy chromaticity and draws
Rec.709, Display-P3 and BT.2020 boundaries. It reports the percentage of
non-black samples outside Rec.709 and Display-P3. This distinguishes declared
primaries from actual gamut use: a BT.2020-tagged file can still contain only
Rec.709 colors.

The vectorscope plots normalized Cb horizontally and Cr vertically, directly
from the decoded chroma planes. This is the digital successor to the analog
NTSC instrument: hue remains the angle around the center and saturation the
distance from it, but the six 75% color-bar targets are calculated from the
file's declared Y'CbCr matrix. Rec.709 and BT.2020 NCL therefore have different
target positions. BT.2020 constant-luminance content is plotted but does not
show NCL target boxes. A conventional 123-degree skin-tone line is included as
a hue guide; it is not a skin detector and is not a normative color target.

The vectorscope trace defaults to 2x display gain because natural content
usually occupies the center of the scope. The target boxes stay fixed and the
scope is clearly labelled `TRACE 2X`; only the trace is magnified. Press `G`
to compare against the unmagnified 1x view, or select the startup value with
`--vector-gain 1|2`. Density bins are deliberately displayed without
smoothing so an 8-bit or otherwise coarsely quantized chroma lattice remains
visible.

The histogram overlays the distributions of encoded R', G' and B' before
transfer conversion, tone mapping or display color management. Its horizontal
axis uses the waveform's -10%..110% signal range, while logarithmic count
height keeps sparse highlight and shadow populations visible beside dominant
tones. It uses 256 signal bins so broad distribution trends remain clear on
high-resolution displays. Values beyond the guard range collect at its end
bins; the panel also reports how many channel samples are outside nominal
0%..100%.

In two-file comparison, the active scope stays inside one pane and follows the
focused (first visible) source.

### Rotation

`--rotate [N:]DEG` turns an input DEG degrees clockwise before display,
where DEG is 0, 90, 180 or 270. A bare `--rotate 90` applies to every
input; `--rotate 1:90` applies to the second file only. The flag is
repeatable, so `--rotate 0:90 --rotate 1:270` sets each file separately.
`T` rotates the focused pane live.

Command-line input indices are 0-based, while the `1` and `2` solo keys are
user-facing and therefore 1-based. Container rotation metadata is not read;
use `--rotate` when a file needs correction.

### Comparing two files

A single **PTS master clock** drives both files: each shows the frame in
effect at that instant. Two files at different frame rates therefore
land on different frame *indices* at the same *moment*, which is what
synchronized has to mean when the rates differ — frame-index lockstep
would drift them apart linearly. A shorter file holds its last frame
instead of going black.

Soloing with `1` or `2` only changes what is rendered. Both sources keep
following the same master clock during playback, seeking and frame stepping,
so returning to comparison with `0` or `P` cannot reveal a stale hidden source.

With two files the default split becomes the **layout**, so `H`/`S` apply to
both panes and you compare A-vs-B under HDR, then A-vs-B under SDR. The
comparison cycle also includes full-frame left/right, top/bottom and
diagonal wipes; these align both complete images and replace half of A
with B, which is especially useful for gradients and banding tests.
Varying content and treatment at once would leave any difference you see
with two possible causes. Press `1` or `2` to solo a file; `P` or `0`
returns to the two-file comparison.

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

Press `D` for a full-frame pixel difference. With one file this compares each
frame to its immediate predecessor (the first frame is shown normally); with
two files it compares A against B. Both frames first pass through
the same selected HDR or SDR display treatment, then the GPU takes their
absolute RGB difference in linear display light and amplifies it 4x. Black
means the displayed pixels match; brighter or coloured areas expose luma or
channel differences. Only the image area covered by both files is compared,
so unequal aspect ratios do not light up the letterbox bars. `C` can still
select Y, Cb or Cr before the difference, and zoom/pan remain locked.

### Aspect ratio and 1:1

The source aspect ratio is always preserved; unused pane space is black. In
two-pane layouts, images are aligned toward the seam. Wipe modes instead
align both complete images and replace part of one with the other.

`Z` toggles fit and 1:1, while `+` and `-` change zoom. Here 1:1 means one
source pixel per framebuffer pixel. The HUD reports the active scale, and `W`
resizes the window so the focused source fits at exactly 1:1 without cropping
or letterboxing. Pan and zoom remain spatially aligned when comparing files
of different resolutions.

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

The report covers luminance distribution, dynamic range, MaxCLL/MaxFALL
consistency, gamut usage and scan coverage. The exit code is the number of
failed content checks; values of 64 or greater indicate a tool error such as
an unreadable file or unsupported pixel format.

Interpretation notes:

- **Measurements are one-sided lower bounds.** Sampling stride, luma vs
  the spec's `max(R,G,B)`, and Jensen's inequality on a convex EOTF all
  push the estimate *down*. So `measured > declared` is real evidence of
  under-declaration and gets a FAIL, while `measured <= declared` proves
  nothing and is reported as INFO — never PASS. `--analyze` defaults to
  `--stride 1` (every pixel) precisely so the FAIL side is sound.
- **HLG numbers rest on an assumption.** HLG carries no absolute
  luminance; converting scene light to display light needs a nominal
  peak `L_W`, applied through the BT.2100 OOTF — a power, not a gain,
  so it changes both absolute brightness and contrast. `--hlg-peak`
  overrides `L_W` for both libplacebo playback and measurement. Without
  an override, playback follows the mapped source/libplacebo policy;
  measurement takes the file's mastering-display max when present, else
  the BT.2100 reference of 1000 nits, and reports which value it used.
- **Ambient adaptation is an explicit hdrplay policy, not a standard.**
  `--reference-lux` overrides the file's AMVE reference environment;
  otherwise AMVE is used when present. On untagged HLG, explicitly supplying
  `--ambient-lux` selects a clearly labelled 314-lux fallback reference, so
  the control remains useful without metadata. `--ambient-lux` supplies the
  current room level; on supported MacBooks hdrplay reads the built-in sensor
  when a source reference exists and that option is omitted. The sensor is
  polled about once per second during playback; polling pauses with playback.
  Darker-than-reference playback lowers midtones while fixing black and the
  HLG peak, exposing more of the display's contrast without making diffuse
  levels needlessly bright. Neither H.274 AMVE nor BT.2100 specifies this
  mapping.
- **SDR gets no absolute figures at all.** A measured MaxCLL for an SDR
  file cannot exceed 100 nits by construction, so those checks are
  suppressed rather than printed with a caveat. Ratio statistics
  (dynamic range, spread) are still valid and still shown.
- **Dynamic range is reported against a ceiling, because a bare ratio
  is unreadable.** `log2(p99.9/p1)` only means something when the
  transfer has a floor. PQ has one by definition; SDR's comes from the
  BT.1886 black level, default 0.1 nits (reference 100-nit monitor,
  1000:1), which caps SDR at a physical 10 stops. Set it with
  `--sdr-black`. At `--sdr-black 0` the transfer is the bare
  `100·V^2.4`, whose slope at the origin is infinite: the darkest
  non-black code tends to zero and the ratio diverges toward the
  quantization limit — 18.7 stops on 8-bit limited range — regardless
  of what the picture contains. The printed ceiling comes from the
  transfer, bit depth and signal range, so "9.0 of 9.7 possible" says
  the file uses its format. A measurement *above* the ceiling is a
  WARN, and it is a measurement finding rather than a content one: p1
  has landed in the code lattice.
- **An absent range flag is recovered from the pixels.** The standard
  reading of a missing `video_full_range_flag` is limited, but getting
  it wrong on a full-range source is not cosmetic — codes 1..15 get
  counted as black and everything just above has its signal value
  divided by roughly ten, which is exactly where the shadow
  percentiles live. A limited-range encode has a hard floor at code 16
  and a hard ceiling at 235, with only a few codes of ringing outside;
  a full-range source with any real black or white does not. hdrplay
  pools that excursion over the first frames and says what it decided
  and on what evidence. Force it with `--range limited|full`.

`--json` writes a machine-readable summary to stdout while checks remain on
stderr. `--stats-file out.ndjson` writes per-frame measurements and session
histograms for external analysis.

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

Unrecognized keys are ignored.

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

## Troubleshooting

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

## Limitations

- Video only: no audio or subtitles.
- No recovery from mid-stream format changes.
- Container rotation metadata is not applied automatically.

## Developer documentation

**Read [`RENDERING.md`](./RENDERING.md) before changing anything in
`renderer.c`.** It documents the color pipeline, intermediate textures,
libplacebo behavior and HDR/SDR brightness handling.

## Files

| File | What it does |
|---|---|
| `src/main.c`        | arg parse, event loop, frame pump, key handling |
| `src/decoder.c`     | ffmpeg demux + decode, HDR side-data extraction |
| `src/renderer.c`    | SDL3, Vulkan and libplacebo initialization and rendering |
| `src/hud.c`         | status panels, labels and video-scope overlays |
| `src/diagnose.c`    | `--diagnose` HDR sanity checks (per-display PASS/WARN/FAIL) |
| `src/brightness.c`  | `--set-brightness` via IOKit / `brightness` CLI / m1ddc |
| `src/probe.c`       | source probes, scope data and luminance statistics |
| `src/stats.c`       | session accumulation: PTS-deduped histograms, percentiles, spread decomposition |
| `src/layout.c`      | pure render planning: passes, crops, masks, overlay routing (GPU-free, so it can be tested) |
| `src/source.c`      | one input: decode, frame ring for step-back, clock following |
| `src/analyze.c`     | `--analyze` headless whole-file scan, `--json`, `--stats-file` |
| `src/checks.c`      | shared PASS/WARN/FAIL reporting for `--diagnose` and `--analyze` |
| `tests/`            | probe, accumulator, layout and source tests (`ctest --test-dir build`) |
| `RENDERING.md`      | rendering and color-pipeline design notes |
| `CMakeLists.txt`    | pkg-config find + link |
