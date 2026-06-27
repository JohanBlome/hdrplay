# HDR Rendering Architecture and Gotchas

This document explains how `hdrplay` actually renders HDR and SDR content
on an HDR-capable display, why the architecture looks the way it does, and
the non-obvious libplacebo behaviors we discovered while building it.

Read this *before* changing anything in `renderer.c`. Several of the
rendering choices look unnecessarily indirect at first glance; they are
not — each indirection was added to work around a specific failure mode
that's documented below.

---

## 1. What hdrplay tries to be

An **insight tool**, not a media player. The goal is to make every step
of the HDR pipeline visible and diff-able:

- What the file claims (decoder metadata)
- What libplacebo decides (tone-map params, gamut, peak)
- What the swapchain negotiates (HDR signalling, EDR headroom)
- What the OS / panel actually does (display state, brightness)

Anything that would be invisible inside a normal player (mpv, ffplay,
QuickTime) needs to be surfaced via tagged stderr logs, the on-screen
HUD, the diagnostics mode, or the luminance probe.

Non-goals: A/V sync, audio, seeking, codec breadth. None of these would
add insight; all of them would add complexity that hides the parts we
care about.

---

## 2. The pipeline, at 10,000 feet

```
   ┌──────────┐    ┌───────────┐    ┌──────────────┐    ┌─────────────┐
   │  file    │ →  │  ffmpeg   │ →  │  libplacebo  │ →  │  Vulkan     │
   │  on disk │    │  demux +  │    │  color +     │    │  swapchain  │
   │          │    │  decode   │    │  tone-map +  │    │  (via SDL3) │
   │          │    │           │    │  render      │    │             │
   └──────────┘    └───────────┘    └──────────────┘    └─────────────┘
                                                                │
                                                                ▼
                                                       ┌─────────────────┐
                                                       │  OS compositor  │
                                                       │  (CoreAnimation │
                                                       │   / Wayland     │
                                                       │   / DWM)        │
                                                       └─────────────────┘
                                                                │
                                                                ▼
                                                       ┌─────────────────┐
                                                       │     panel       │
                                                       └─────────────────┘
```

Per-layer responsibilities:

- **ffmpeg**: demux, decode, attach HDR side-data (mastering display
  metadata, MaxCLL/MaxFALL, Dolby Vision RPU). Outputs `AVFrame` with
  populated `color_primaries / color_trc / color_space / color_range`.
- **libplacebo**: upload `AVFrame` planes to GPU, color-decode YUV→RGB,
  apply tone-mapping (10000 → panel-peak nits), gamut-map between
  primaries, encode for target color space, dither, render to texture.
- **Vulkan + SDL3**: get a Vulkan instance with the right extensions
  (`VK_KHR_portability_enumeration` on macOS for MoltenVK), create a
  Vulkan surface tied to an HDR-signalling SDL3 window, present.
- **OS compositor**: take the HDR-tagged swapchain image, blend it into
  the desktop scene, signal HDR to the display per its protocol.
- **Panel**: decode PQ codes back to absolute nits, display.

The "interesting" layer for color correctness is libplacebo; the
"interesting" layer for HDR signaling is the swapchain/OS boundary.

---

## 3. The display reality on macOS

macOS uses **Extended Dynamic Range (EDR)** for HDR composition.
Conceptually:

```
linear brightness scale (relative)
0.0 ───────── 1.0 ─────────────────── (headroom) ───────── (panel max)
black        SDR white                                       HDR peak
```

- **SDR white** is a reference luminance (≈ 100–500 nits depending on
  display preset and brightness slider).
- **EDR headroom** is the multiplier above SDR white that the panel can
  currently deliver — `panel_peak / SDR_white`.
- The headroom is **dynamic**: it changes when ambient light changes,
  when you slide the brightness, when battery / Low Power Mode toggles,
  when you change reference modes.

SDL3 exposes both via per-window properties:
`SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT` and
`SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`. `hdrplay` polls these every frame
into `Renderer::display_sdr_white` / `display_hdr_headroom` and surfaces
them in the HUD.

**Key consequence for HDR-correct rendering**: there is no fixed
"absolute" peak luminance on macOS at any given moment. We can ask for
HDR signalling, but the panel's actual response depends on user-mutable
state we can only observe, not control. This is why diagnosis and
warnings are first-class features (see `--diagnose`), and why the HUD
turns `HEADROOM` red above 8× (dark content becomes invisible at low SDR
white / high headroom).

---

## 4. The core architectural insight: SDR via overlay, not via swapchain

> **This is the most important section in this document.** Every
> subtle bug we hit during development reduced to this.

### The problem

We want to render the same source three different ways:

1. **HDR mode** — full panel peak, all the headroom we have.
2. **SDR mode** — tone-mapped to a fixed 203-nit ceiling, looks like
   SDR-on-an-HDR-display.
3. **Split** — half of each (LR / TB / diagonal) for direct comparison.

Naive implementation: call `pl_render_image` once or twice with
`target.color.hdr.max_luma` set to `panel_peak` (for HDR) or `203` (for
SDR). This **does not work**. Both modes display at panel peak.

### The non-obvious behavior

`libplacebo` has **two distinct rendering paths** with different rules
about color metadata. The public API does not flag this asymmetry; it
took us many iterations to triangulate it.

| Path | What honors `target.color.hdr.max_luma` for absolute brightness |
|---|---|
| `pl_render_image` with **target wrapping a swapchain frame** | Treated as a *tone-map hint*. The swapchain's surface colorspace (negotiated by the OS) controls the actual output encoding. The tone-mapped pixels are normalized into the swapchain's full PQ code range — so "203 nits requested" lands at panel peak. |
| `pl_render_image` with **target wrapping our own RGBA texture** | Honored. Pixels are written at absolute PQ encoding (e.g. 203 nits → PQ code ≈ 0.578). |
| `pl_overlay` composition (intermediate texture attached as overlay to a swapchain render) | Bypasses the tone-mapping pipeline entirely. The overlay's pixel values are alpha-blended into the swapchain in the overlay's declared color space, preserving the absolute brightness baked into the intermediate. |

### The implication

To get **correct absolute SDR brightness inside an HDR swapchain frame**,
we cannot render SDR directly to the swapchain. We must:

1. Render SDR into a **plain RGBA16F intermediate texture** (where
   `target.color.hdr.max_luma = 203` is honored — pixels are encoded
   absolutely at 0–203 nits).
2. Render HDR full-frame to the swapchain normally.
3. Attach the intermediate as a **`pl_overlay`** with a per-pixel alpha
   mask determining where SDR shows vs HDR.

This is the same pattern mpv and other libplacebo-based players use for
SDR-look rendering on HDR displays.

### How this manifested as bugs we chased

The asymmetry hid the bug because **the diagonal split mode used the
overlay path from day one** (it's the only way to get a smooth diagonal
without a stair-stepped strip approximation). LR / TB / full-SDR modes
all used direct-swapchain rendering. So:

- "Diagonal shows a brightness difference, LR/TB don't" — the overlay
  path actually worked. The direct-swapchain path silently dropped our
  SDR brightness override.
- "Switching between HDR and SDR mode shows no difference" — same root
  cause. Both modes were rendering at panel peak; only the tone-map
  *curve* was different, and the curve difference was invisible because
  the file's actual content didn't trigger meaningful highlight
  compression.

The current architecture (`renderer.c`'s unified composite path) uses
the overlay pattern for **all** SDR-related modes. Plain SDR mode has a
fully-opaque alpha mask, LR/TB modes have hard binary masks, diagonal
has a smoothstep mask. One pipeline, four mask shapes.

---

## 5. The rendering modes

All paths share a single per-frame `pl_render_image` invocation against
the swapchain (with HDR target). SDR content piggy-backs as an overlay.

```
                  ┌─────────────────────────────────────────┐
                  │   Per-frame entry: renderer_render_avframe │
                  └─────────────────────────────────────────┘
                                     │
       ┌─────────────────────────────┼─────────────────────────────┐
       ▼                             ▼                             ▼
 ┌──────────┐                ┌──────────────┐                ┌──────────────┐
 │ HDR mode │                │  SDR mode    │                │ SPLIT modes  │
 │          │                │              │                │ (LR/TB/DIAG) │
 │ 1 render │                │ 2 renders:   │                │ 2 renders:   │
 │   to     │                │   src→inter  │                │   src→inter  │
 │ swap     │                │   src→swap   │                │   src→swap   │
 │          │                │   + overlay  │                │   + overlay  │
 │          │                │ mask = FULL  │                │ mask = LR /  │
 │          │                │              │                │  TB / DIAG   │
 └──────────┘                └──────────────┘                └──────────────┘
```

| Mode | First render | Second render | Overlay mask |
|---|---|---|---|
| `HDR` | source → swapchain (HDR target) | — | none |
| `SDR` | source → intermediate (SDR target) | source → swapchain (HDR target) | `FULL` (alpha=1 everywhere) |
| `SPLIT-LR` | source → intermediate (SDR target) | source → swapchain (HDR target) | `LR` (alpha 0 left, 1 right) |
| `SPLIT-TB` | source → intermediate (SDR target) | source → swapchain (HDR target) | `TB` (alpha 0 top, 1 bottom) |
| `SPLIT-DIAG` | source → intermediate (SDR target) | source → swapchain (HDR target) | `DIAG` (smoothstep diagonal) |

The intermediate render uses `pl_blend_params { src_rgb=ONE, dst_rgb=ZERO,
src_alpha=ZERO, dst_alpha=ONE }` so the SDR render writes RGB but
**preserves** the pre-uploaded alpha mask. The mask is computed once on
the CPU and re-uploaded only when the window size or mode/orientation
changes.

The split orientation cycles `LR → TB → DIAG → LR` via the `O` key.
Switching invalidates `diag_tex.mask_mode`, which triggers a one-frame
mask regeneration on the next render.

---

## 6. libplacebo gotchas, in the order we learned them

A field guide. Each of these cost ≥ 1 iteration to diagnose.

### 6.1 `pl_dispatch_info.pass` has no `desc`

In libplacebo 7.x, the pass description lives at
`info->pass->shader->description`, not `info->pass->desc`. The 6.x docs
suggest the latter. Symptom: compile error on `pass->desc`. Fix: dig
through `pl_shader_info`'s `description` field, which is a
comma-separated string of semantic steps.

### 6.2 `pl_gpu_t` has no `name` field

In 7.x, the public GPU struct doesn't expose a device name string.
Query Vulkan directly via `vkGetPhysicalDeviceProperties` on
`r->vulkan->phys_device` for `deviceName`. (On Apple Silicon this is
"Apple M*" via MoltenVK.)

### 6.3 `pl_map_avframe_ex` requires a persistent texture array

For non-hwdec frames (anything decoded into CPU memory), `pl_avframe_params.tex`
must point to a caller-owned array of 4 `pl_tex` slots. libplacebo
(re)creates textures into these slots and reuses them across frames.
Pass `NULL` and `pl_map_avframe_ex` silently fails every frame.

Store the array on `Renderer::plane_tex[4]` and destroy on
`renderer_close`.

### 6.4 Overlays must be attached BEFORE `pl_render_image`

`pl_render_image` composites overlays as part of its render pass. If
you set `target.overlays` *after* calling `pl_render_image`, the
overlays don't render. The HUD code originally set overlays after the
main render — invisible for many iterations until I noticed the user
had simply never been seeing the HUD.

### 6.5 Border/clearing semantics changed in v7.346

`pl_render_params.skip_target_clearing` is deprecated; use
`border = PL_CLEAR_SKIP` instead. Important when overlaying a second
render onto a partially-rendered swapchain (e.g. split modes): without
`PL_CLEAR_SKIP`, the second render's default border behavior paints
background-color over the first render's pixels.

### 6.6 `pl_color_space_hdr10` defaults `max_luma` to 0

The named preset declares primaries=BT.2020, transfer=PQ, but leaves
`hdr.max_luma = 0`. libplacebo's logic for unset `max_luma` is heuristic
and not what you want for our case. Always explicitly set `max_luma` to
the intended value on any `pl_color_space` you construct.

### 6.7 The asymmetry between swapchain targets and texture targets

The big one. See section 4 above. Documented behavior in
`pl_render_image` does not flag it. We had to triangulate.

### 6.8 Shader caches are per-`pl_renderer` instance

`pl_renderer` caches compiled shaders against the `(source, target,
params)` tuple it last saw. Ping-ponging a single renderer between two
different target colorspaces (HDR pass and SDR pass, both per-frame)
caused 50–100 ms of shader recompilation per pass on macOS via MoltenVK.

Fix: instantiate **two separate `pl_renderer`s**, one per consistent
target configuration. `renderer.h` calls them `renderer` (HDR / main)
and `renderer_sdr` (SDR-to-intermediate). Each keeps its compiled
shaders warm.

This is the same pattern mpv uses for its HDR/SDR layer rendering.

### 6.9 Overlays are clipped to `target.crop`

When you render to a subregion of the swapchain (e.g. a split half) and
attach an overlay, the overlay is clipped to that subregion's crop. An
overlay positioned in the right half won't render if attached to a
left-half render. In split modes, attach overlays to the render whose
crop contains the overlay's destination rect.

### 6.10 RGBA16F precision matters for the SDR intermediate

The intermediate texture stores PQ-encoded SDR content (0–203 nits ≈ 0
to PQ-code 0.578). 8-bit storage gives ~130 effective levels in that
range, which produces visible banding on smooth gradients. RGBA16F is
fine. RGBA10A2 would also work but is slightly more fiddly to allocate
across backends.

### 6.11 The `info_callback` last-tone-map heuristic is misleading

`pl_info_cb` fires for every shader pass. Our log code captured "the
last pass whose description contains 'tone'". Overlay composition has
its own tone-map step that often comes last — so the captured string
describes the *overlay's* tone-map, not the *frame's* tone-map. This
made the per-frame log look identical between HDR and SDR mode even
when the underlying renders differed (or didn't).

If you need to verify the *frame's* tone-map is changing per mode, you
have to instrument earlier in the callback chain (e.g. capture only the
first matching pass, or capture all of them).

---

## 7. The MoltenVK story (macOS only)

Vulkan on macOS isn't built-in. We need MoltenVK (Khronos's
Vulkan-on-Metal portability driver) plus the Vulkan loader. Three
things:

1. **Loader extension**: enable `VK_KHR_portability_enumeration` and
   `VK_KHR_get_physical_device_properties2` on instance creation.
   Without these the loader silently skips MoltenVK and reports "no
   physical devices" — even though MoltenVK is right there.
2. **ICD discovery**: the Vulkan loader needs a JSON manifest pointing
   at `libMoltenVK.dylib`. Homebrew doesn't ship one in a default search
   path. We bundle MoltenVK under `third_party/` and have `main.c`'s
   `ensure_moltenvk_icd()` set `VK_ICD_FILENAMES` to the bundled JSON
   if the env var isn't already set.
3. **MoltenVK noise**: `MVK_CONFIG_LOG_LEVEL=1` to suppress the
   150-line extension dump at startup (escalated to `3` under `-v`).

---

## 8. Diagnostic tools and what to look for

When something looks wrong, use these in order:

### 8.1 Per-frame stderr (greppable)

```
[DEC]   demux/decode events
[META]  HDR10/HDR10+/DV metadata on the source
[GPU]   Vulkan + libplacebo init
[SWAP]  swapchain colorspace negotiation
[HDR]   display HDR state (headroom, SDR white)
[REND]  per-frame render decisions
```

Each tag is greppable. `[REND] frame:` lines summarize each frame's
output target.

### 8.2 The on-screen HUD

Top-left status panel shows:
- `MODE`: HDR / SDR / SPLIT LR / SPLIT TB / SPLIT DIAG
- `DISPLAY HDR ON/OFF  HEADROOM Nx` (red and warning when headroom > 8×)
- `src: prim=N trc=N peak=Nn` — what the source claims, libplacebo's
  view
- `out: ...` — what we asked the swapchain / overlay to render
- `FRAME N PAUSED/LOOP` — playback state
- `PROBE` — luminance probe readout (green) when active

### 8.3 The luminance probe (`M` key)

Samples the source AVFrame pixel under the cursor, runs YUV→RGB plus
inverse transfer to get nits. Reports source-side absolute nits — not
output brightness. Use this to confirm that "what's missing on the
panel" actually exists in the file.

### 8.4 `--diagnose`

Runs sanity checks against the chosen display (or all displays):
- HDR enabled on display (system + window-level)
- EDR headroom in a reasonable range
- Mirroring inactive (mirroring disables HDR)
- Power source (battery / Low Power Mode warning)
- Reference-mode inference from headroom

Exit code = number of FAILs, so it can gate a test pipeline.

### 8.5 `--list-displays`

Enumerates displays with HDR-capable flag and bounds. Pair with `-d N`
to pick a specific monitor for the playback window.

---

## 9. Known limitations and "but-what-about" answers

### Why doesn't SDR brightness match ffplay / QuickTime?

It does now, by default. But the underlying reason is interesting and
worth understanding for anyone tweaking the SDR peak.

**Native macOS apps (QuickTime, Safari, Finder, ffplay)** write SDR
content into an SDR-tagged surface. macOS's SDR layer compositor then
applies an EDR brightness boost so SDR sits at the panel's "SDR
reference white", which on HDR-enabled Macs is typically **400–600
nits** depending on display preset and brightness slider. Apple's
stated design goal: "SDR shouldn't look dim on HDR displays."

**hdrplay** writes everything into an HDR-tagged surface (we have to —
that's how we get HDR output for HDR mode). For SDR content, we tone-
map to an absolute nit ceiling and PQ-encode at that ceiling. There is
no implicit EDR boost; what we ask for is what the panel shows.

We default the OS-tracked SDR peak to `SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT
× 500` ≈ 500 nits in typical setups. We also enable libplacebo's
**`inverse_tone_mapping`** flag in the SDR pass. This second piece is
critical for **SDR-source content**: by default libplacebo preserves
absolute brightness when source max ≤ target max, so a 100-nit SDR
file rendered into a 500-nit SDR target stays at 100 nits — and looks
much dimmer than macOS's own SDR composition of the same file.
`inverse_tone_mapping` tells libplacebo to expand the source's
dynamic range up to the target ceiling, mimicking macOS's EDR boost.

For HDR-source content the flag is a no-op (libplacebo still tone-
maps down to fit the SDR ceiling). Override the brightness via
`--sdr-peak`:

| Value | What you're asking for |
|---|---|
| `--sdr-peak 100` | Strict BT.2100 spec. Dim — useful for "this is what reference SDR really is" |
| `--sdr-peak 203` | BT.2408 HDR Video diffuse-white reference. Moderate. |
| `--sdr-peak 500` (default) | Perceptually matches QuickTime / Safari / typical macOS SDR composition |
| `--sdr-peak 800` | Apple Display preset / bright SDR look |

### Why is dark content invisible in HDR mode on bright rooms?

When EDR headroom > ~8×, macOS is in "max stretch" mode and SDR white
sits below 25 nits. The bottom 2 stops of HDR signal land below typical
ambient-light reflectance threshold. The HUD turns `HEADROOM` red and
emits a warning when this state is detected. Either dim the room or
slide the panel brightness up to reduce headroom.

### Why are wide-gamut colors not clipped in SDR mode?

We tone-map brightness (10000 → 203 nits) but do **not** gamut-map
(BT.2020 → BT.709). Wide-gamut content on the SDR side will look more
colorful than true SDR. The fix is straightforward (pass an explicit
`pl_gamut_map_params` with `input=BT.2020 output=BT.709` via
`pl_render_params.color_map`), just not yet implemented. The lack of
gamut clamping is the reason we keep `apply_sdr_target` from overriding
`target.color.primaries` — overriding it without proper encoding
support produced the over-saturated colors we saw in earlier
iterations.

### Why is there a stair-stepped diagonal fallback comment in `renderer.c`?

The original `--split-diag` implementation used 24 horizontal strips of
alternating HDR and SDR renders. It worked but was visibly stair-stepped.
The current overlay-mask approach is strictly better. The comment is
historical; the strip code is gone.

### Why two `pl_renderer` instances?

Shader cache isolation. See 6.8.

### Why is the intermediate texture called `diag_tex` even when used for non-diagonal modes?

Historical name. It started life as the diagonal-split intermediate and
got generalized when we discovered the overlay path was the only thing
that worked for any SDR rendering. Renaming to `sdr_intermediate_tex`
would be more honest; leaving it for now to avoid churn.

---

## 10. References

- libplacebo public API: <https://github.com/haasn/libplacebo/tree/master/src/include/libplacebo>
- libplacebo's own demo player: `demos/plplay.c` in the libplacebo
  repo. This was the closest thing to a working reference and the
  source of several of the patterns above (separate renderers, overlay
  composition).
- SDL3 HDR window properties: <https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties>
- MoltenVK: <https://github.com/KhronosGroup/MoltenVK>
- macOS EDR developer documentation:
  <https://developer.apple.com/documentation/metal/hdr_content/applying_extended_dynamic_range_to_hdr_content>
- BT.2100 (PQ + HLG spec): <https://www.itu.int/rec/R-REC-BT.2100>
- BT.2408 (HDR diffuse-white reference = 203 nits):
  <https://www.itu.int/pub/R-REP-BT.2408>
