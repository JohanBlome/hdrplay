# Side-by-side comparison of two files

**Date:** 2026-08-28
**Status:** **implemented** — see [As built](#as-built)

## As built

Shipped as `src/layout.{c,h}` and `src/source.{c,h}`, plus
`tests/test_layout.c` and `tests/test_source.c`, with the render path in
`renderer.c` rewritten around the plan and `main.c` rebuilt around the
master clock. All three test binaries pass; the build is now
warning-free, which it was not before.

Five things diverged from the design:

1. **`Source` does not own GPU state.** `norm_tex`/`sdr_tex` moved to
   `Renderer.slot[]`. Keeping libplacebo out of `source.h` means the
   source and ring logic can be unit-tested without a GPU — which is
   what `tests/test_source.c` relies on.

2. **The clock needs a one-frame lookahead.** You cannot know whether a
   frame is due without decoding it first, so `Source` carries a
   `pending` frame alongside `shown`. The design's "advance until the
   next PTS exceeds the clock" glossed over this.

3. **Layout owns panel PLACEMENT, not just routing.** The design had
   layout deciding which pass each panel belongs to while `hud.c` kept
   positioning them. That would have put the same rect arithmetic in two
   files, free to drift. `hud_prepare` now walks the plan and draws each
   panel at the rect layout assigned.

4. **Geometry normalization became a per-source crop, not an extra
   render pass.** The design called for upscaling the smaller source
   into a normalization texture so both panes share a resampling path.
   What shipped instead computes the visible region as a FRACTION of the
   reference (larger) source and resolves it into each source's own
   pixels.

   This delivers the property that matters — both panes always show the
   same region of the scene, so at 1:1 a 1080p pane no longer covers
   four times the area of a 4K one — without a second render pass and
   without the double-tone-mapping hazard that feeding a rendered
   texture back in as a source would create (RENDERING.md §6.6).

   What it does NOT do is equalize the resampling ratio. The design's
   argument for that was fairness: scaling 4K and 1080p into
   identically-sized panes puts them through different scale factors.
   That argument is weaker than it looked, because at 1:1 zoom — the
   mode you actually use to compare detail — neither pane is resampled
   at all, and in fit mode the comparison cannot resolve fine detail
   regardless. **Left undone deliberately**; revisit if fit-mode
   comparison of mismatched resolutions turns out to matter.

5. **A latent bug fell out of doing (4) properly.** The first cut used
   one `src_w`/`src_h` for both panes, so with mismatched resolutions
   pane B received a crop expressed in pane A's pixel space — silently
   wrong, and only visible once zoomed. `LayoutInput` now carries
   per-source geometry and `tests/test_layout.c` covers the 4K-vs-1080p
   case.

Also cleaned up on the way through: the `AlphaMaskMode` enum moved out
of the `Renderer` struct (it was the source of a `-Wmissing-declarations`
warning on every translation unit including `renderer.h`), and two dead
statics went with the render rewrite.

## Problem

hdrplay compares *renderings* of one file. It cannot compare two files.

The gap matters because the tool's usual job — "is this encode good?" —
is answered by holding two encodes next to each other, not by looking at
one in isolation. Today that means two windows, two playheads, and no
way to land on the same frame in both.

## Goals

- Play two files synchronized, side by side, in one window.
- Step forward and backward a frame at a time.
- Keep the existing HDR-vs-SDR comparison reachable.

## Hard constraint

**Single-file playback must behave exactly as it does today.** Same
render passes, same modes, same keys, same panels. The two-source
machinery only engages when a second file is present. This is enforced
by a test, not by care — see [Testing](#5-testing).

## Non-goals

- More than two files. The split is the layout, so two is the limit.
- Audio, or A/V sync of any kind.
- Difference/heatmap views (A−B). Possible later on the same plumbing.
- Any change to `--analyze` beyond accepting two paths.

## The naming collision, and why it forces a decision

Today **split is a rendering comparison**: one decoded frame is rendered
twice — HDR to the swapchain, SDR into `diag_tex` — and composited with
an alpha mask (`src/renderer.c:702-760`). Both halves are the same
pixels; only the treatment differs.

What we are adding is a **content comparison**: two files, one per half.
Different axis, same word.

So in two-file mode the split is spent on the layout, and `H`/`S` can no
longer mean "HDR one side, SDR the other". They apply to **both** panes
uniformly, letting you compare A-vs-B in HDR, then A-vs-B in SDR.

That is the right constraint anyway: varying content *and* treatment
simultaneously means any difference you see has two possible causes,
which is the opposite of what a comparison tool is for. The HDR-vs-SDR
view stays reachable via solo (`1` / `2`), which drops back to exactly
today's single-file behaviour on that file.

---

## 1. Source model and the master clock

`main.c` currently owns one `Decoder` and drives it directly. That
becomes an array of 1–2 `Source`:

```c
typedef struct {
    Decoder      dec;
    const char  *label;          /* basename, for the pane badge      */
    AVFrame     *shown;          /* frame currently on screen         */
    bool         eof;

    AVFrame    **ring;           /* recent frames, for step-back      */
    int          cap, len, head;

    pl_tex       norm_tex;       /* geometry normalization, if needed */
    pl_tex       sdr_tex;        /* per-source SDR intermediate       */

    SessionStats session;        /* stats are per-file                */
    FrameStats   frame_stats;
    bool         frame_stats_valid;
} Source;
```

`diag_tex` moves out of `Renderer` and into `Source` as `sdr_tex`. One
shared texture would be a GPU hazard once two passes are queued.

### The clock is the sync

One `double clock_sec`. Each source independently advances until the
next frame's PTS would exceed the clock, and displays the last frame
whose PTS is `<=` it — "the frame in effect at time *t*".

Consequences, all of them wanted:

- Two files at 24 and 30 fps land on **different frame indices at the
  same instant**. That is what synchronized means when rates differ.
- A source that reaches EOF early stops advancing and holds its last
  frame, rather than going black.
- Files with a container start-time offset line up by wall-clock time.

Playback advances the clock from wall time, reusing the existing pacing
(`src/main.c:571`). This *replaces* per-decoder pacing rather than
layering on top of it.

### Stepping moves the clock, not the decoders

Step forward advances the **reference** source to its next frame, sets
`clock` to that frame's PTS, and lets the other source follow. Reference
is A in two-file mode, or the soloed file.

So a step is always exactly one frame on the side you are looking at,
and the other side moves 0, 1 or 2 frames as its rate demands. Stepping
auto-pauses.

Step back pops the ring buffer. Below `len`, it seeks to the preceding
keyframe and decodes forward, refilling the ring on the way so repeated
back-steps stay fast.

`--step-buffer N` sets the ring depth, default 8, `0` disables it.
Frames are retained by reference (`av_frame_ref`), not copied, but the
memory is real: ~25 MB per 4K 10-bit frame, ~6 MB at 1080p. Eight frames
across two files is ~400 MB at 4K, ~100 MB at 1080p.

## 2. Rendering two sources in one window

Note the irony: the stale `hud.c` comment removed in the previous change
described a two-pass, per-half-crop architecture. It was wrong for one
file, and it becomes **true again** for two. Per-pane overlay routing
returns as a real requirement.

### LR and TB: two passes with rectangular crops

Pane A renders source A with `target.crop` = left/top half; pane B
renders source B into the other half. Direct, no intermediate.

This is deliberately *not* the overlay path, because the overlay
compositor silently renormalizes PQ codes unless `max_luma`,
`min_luma` and `primaries` are hand-matched — the footgun documented at
`src/renderer.c:507-522` and in RENDERING.md §6.6. Rendering each pane
directly sidesteps it and avoids a full-resolution round-trip.

### DIAG: the alpha-mask overlay path

A diagonal wipe cannot be expressed as a crop, so DIAG keeps today's
machinery: source B renders into its own intermediate carrying the
diagonal mask, then composites over A.

This is a clean generalization — the existing code already composites
"a second image with an alpha mask", it just happens to be the SDR
version of A today. Two-file mode swaps what that second image is.

### SDR mode

Each pane routes through its own `sdr_tex` with the existing
`render_sdr_to_intermediate` treatment, then composites. Same reason
`sdr_tex` is per-source.

### HUD routing

Each panel is assigned to the pass whose crop contains it — deterministic,
not "attach to everything". Attaching one overlay to two passes would
double-blend it (alpha 170 → ~0.89), which is exactly the defect the
design review caught in the accumulated-stats plan.

In two-file mode under LR the session panel moves to bottom-**left** so
it stays inside pane A. Nothing straddles a seam.

### Zoom

Falls out for free: `image.crop` is set to the panned sub-rect of the
normalized source, shared between panes.

## 3. Geometry normalization

The reference geometry is the larger source by pixel area. Any smaller
source gets one extra `pl_render_image` into `norm_tex` at that geometry
before its pane render; differing aspect ratios letterbox into the
reference box rather than stretching. The larger source is untouched,
and when the two files match — the common case — there is no extra pass
at all.

**Why bother:** it makes both panes share a resampling path. Scaling a
4K source and a 1080p source directly into identically-sized half-panes
puts them through different scale ratios, so part of what you would see
is the scaler rather than the encode. Normalizing first makes that
difference common-mode.

It also means the upscale is *visible*, which is correct: comparing a
1080p encode against a 4K master, what you want to judge is how the
1080p holds up when displayed at 4K.

**Known limitation:** a half-pane in a 1080p-tall window is ~960 px
wide, so both sources are downscaled into it regardless. At that size a
4K-vs-1080p difference is largely invisible — you are comparing two
downsampled images. Normalization fixes the *fairness* of the
comparison, not its *resolving power*. That is what zoom is for.

## 4. Controls, HUD, CLI

```bash
hdrplay a.mov              # unchanged, exactly as today
hdrplay a.mov b.mov        # A|B compare, split LR, forced
```

New keys, all additive to the existing set
(`ESC/Q F H S P O SPACE L R ←/→ M I A shift-A`):

| Key | Action |
|---|---|
| `.` / `,` | step forward / back one frame (**both modes**), auto-pauses |
| `0` `1` `2` | A\|B compare / solo A / solo B |
| `X` | swap sides |
| `Z` | toggle 1:1 ↔ fit |
| `+` `-` | zoom steps |
| drag | pan, locked across panes |
| `shift`+arrows | pan by keyboard |

Two collisions, resolved:

- `←`/`→` keep their existing ±10s seek, so panning takes mouse-drag
  and `shift`+arrows instead.
- Drag-to-pan coexists with the `M` luminance probe because the probe
  tracks bare motion while panning requires a held button.

### HUD, two-file mode only

- Badges stop saying HDR/SDR — both panes share a treatment — and show
  file basenames instead.
- `FRAME` shows both indices (`FRAME A:101 B:126`), which is where the
  rate difference becomes visible.
- The session panel becomes a two-column comparison, which is the point
  of the mode:

```
SESSION  COVER 66PCT
            A        B
MaxCLL   4210N    3980N
P50        48N      51N
DR       11.4     10.9
SPREAD    2.1      2.3
```

Statistics accumulate per source, independently, reusing the existing
accumulator unchanged.

### `--analyze` with two files

Analyzes each in turn and returns the summed FAIL count, so the batch
triage loop keeps working.

## 5. Testing

The render path needs a GPU, so the decision-making is extracted into a
pure function:

```c
LayoutPlan layout_plan(mode, orient, n_sources, win_w, win_h, zoom, pan);
```

returning the pass count, per-pass source index, `target.crop`,
`image.crop`, alpha mask, and which HUD overlay belongs to which pass.
Headlessly testable.

**The single-file regression test pins the one-source plan against
today's known-good values.** That is what enforces the hard constraint:
if a two-file change perturbs single-file rendering, the test fails
instead of someone noticing a month later.

Other properties:

- **Sync:** given two PTS lists and a clock, the selected frames are the
  last with PTS `<=` clock. Covers 24-vs-30 fps, and a short file
  holding its last frame past EOF.
- **Ring buffer:** wraparound; step-forward-N-then-back-N returns the
  identical frames.
- **Seek fallback** fires exactly when the request is deeper than `len`,
  and refills the ring.
- **Normalization:** larger-by-area wins; letterbox offsets correct for
  differing aspect; no-op when geometry matches.
- **Zoom/pan:** both panes resolve to the same source rect; clamps at
  edges; `fit` restores exactly the full-frame crop.

## 6. Landing order

The first three are useful standalone and touch nothing about two files.

1. Extract `layout_plan()` + regression tests pinning today's
   behaviour. Pure refactor, no behaviour change.
2. `Source` struct; array of one. Still single-file, still no behaviour
   change. `diag_tex` becomes `sources[0].sdr_tex`.
3. Master clock, ring buffer, `.`/`,` stepping.
   **Ships frame stepping for single files.**
4. Second source: CLI, decode, LR/TB two-pass render, badges, HUD
   routing.
5. DIAG wipe, `X` swap, `0`/`1`/`2` solo.
6. Geometry normalization.
7. Zoom + pan.
8. Two-column session panel, `--analyze` two files, README.

## Decisions

| Decision | Alternatives rejected | Why |
|---|---|---|
| PTS master clock | Frame-index lockstep; PTS + manual offset | Correct when rates differ, which frame-index is not; no offset UI to get wrong |
| Ring buffer + seek fallback | Seek every time; ring with no fallback | Backward stepping is interactive, and a full-GOP pause on every nudge is the wrong feel; fallback keeps the whole clip reachable |
| Split is the layout; H/S apply to both | Mixed A-HDR vs B-SDR | Varying content and treatment at once confounds the comparison |
| Solo `1`/`2` restores HDR-vs-SDR | Relaunch with one file | Keeps "is this an HDR artifact or an encode artifact?" a single keypress |
| Normalize smaller source to larger | Each scales to fit its own half | Puts both panes through the same resampling path, so scaler differences are common-mode |
| Crops for LR/TB, overlay only for DIAG | Overlay path for everything | Avoids the PQ-renormalization footgun and a full-res round-trip on the common orientations |
| Zoom + pan, locked across panes | Fit only; zoom without pan | Without 1:1 the tool can only show gross differences, not the compression artifacts worth hunting |
