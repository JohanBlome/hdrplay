# Accumulated source statistics

**Date:** 2026-08-28
**Status:** **implemented** — see [As built](#as-built) for where reality diverged
**Revision:** v2 — addresses `accumulated-stats-design-review-1.md` (7 critical issues)

## As built

Shipped across `src/probe.{c,h}`, `src/stats.{c,h}`, `src/checks.{c,h}`,
`src/analyze.{c,h}`, `tests/test_stats.c`, with wiring in `hud.c`,
`renderer.c`, `decoder.c` and `main.c`. 51 tests pass
(`cmake --build build --target test_stats && ./build/test_stats`).

Five things the design got wrong or left out, found while building:

1. **Coverage could never reach 100%.** A PTS marks where a frame
   *starts*, so the high-water mark of the last frame sits one frame
   interval short of the duration — a complete scan reported 99% and
   was permanently flagged as partial. `session_stats_add` now takes the
   frame duration and adds it. The design never mentions this.

2. **The C5 example was wrong.** The design claimed near-black PQ codes
   clamp into bin 0 and degenerate p1. Measured: at 10-bit limited PQ,
   `y_raw = 65` is 5.3e-5 nits, which lands in **bin 28** — comfortably
   resolved, because the histogram floor is 2^-16 = 1.5e-5. The
   underflow bucket is still load-bearing, but for **SDR**, where the
   same code is 8.7e-6 nits and does fall below the floor. Both cases
   are now asserted in the tests. The fix was right; the reasoning
   given for it was not.

3. **`ov_arr[5]`, not `[6]`.** The design sized for attaching the panel
   twice, a habit left over from v1's phantom two-pass model. One entry
   is needed, so the worst case is 5 (SDR intermediate + status + two
   split badges + session).

4. **Per-frame CLL had to be shared, not analyze-only.** The design put
   the `av_frame_get_side_data` upgrade in `analyze.c`. That made
   playback and `--analyze` disagree about what the container declares
   on x265 output, which stamps MaxCLL per frame. Lifted into
   `decoder_absorb_frame_side_data()` and called from both.

5. **`--analyze` degrades rather than refusing.** When chroma is
   unreadable (NV12), it falls back to `PROBE_LUMA_ONLY` instead of
   measuring nothing, and reports that MaxCLL is not to spec. The design
   implied an all-or-nothing gate.

Also carried over from the bug-fix pass: the format guard is split into
`luma_plane_supported()` and `chroma_planes_supported()`, so NV12 keeps
its luma statistics.

Verified end to end on generated PQ / HLG / SDR clips. The one-sided
lower bound is observable: the same PQ file reports MaxCLL ≥ 5828N from
playback (luma-only, stride 8) and 10000N from `--analyze` (maxRGB,
stride 1).

## Changes from v1

| # | v1 said | v2 says |
|---|---|---|
| C1 | `eotf()` output is nits for all transfers | HLG gets the BT.2100 OOTF with an explicit `L_W`; every stat carries a luminance-reference tag; absolute checks suppressed for SDR |
| C2 | Split mode has two render passes; attach the panel to both | **There is one pass.** One overlay entry, appended after `sdr_ov`. Grow `ov_arr[4]`→`[6]`. Fix the stale comment that caused this error |
| C3 | `measured <= declared` is `PASS` | It is `INFO / not disproven`. Drop the ×1.5 margin. Add a maxRGB histogram so MaxCLL is measured to spec |
| C4 | Dedupe on `current_frame_no` + frame bitset | **PTS high-water mark.** Deletes the bitset, `nb_frames`, the fps estimate and the whole fallback chain |
| C5 | A zero-counter retires `floor_cutoff_nits` | It does not. Black is defined in the *code* domain; bin 0 is a separate underflow bucket excluded from percentiles; the DR floor moves to p1 |
| C6 | `total² = spatial² + temporal²` is a free self-check | Only if every term is log2-domain over the same population. Fully specified now; `within_var_sum` gets a source |
| C7 | Gate on `comp[0].depth` | Also gate on `shift == 0`, planar, `step == 1`. **P010 currently reads every sample as 10000 nits** |
| — | 256 bins @ 1/8 stop | 512 bins @ 1/16 stop — 1/8 gave ±0.25 stop error on a number printed to one decimal |
| — | no gamut/range checks | Illegal-range and out-of-BT.709 checks added (user decision) |

## Problem

`probe_frame_stats()` (`src/probe.c:164`) computes peak / avg / floor /
`dr_stops` / percentile counters for the current frame. The renderer
refreshes it on every decoded frame (`src/renderer.c:630`) and the HUD
prints it live (`src/hud.c:264`). Nothing survives the frame.

That answers "is *this frame* HDR?" but not **"is this file worth using
as an HDR test clip?"**

## Goals

- Accumulate source statistics across frames, surfaced live in the HUD.
- Emit a whole-file verdict from a headless `--analyze` pass.
- Never present a partial or biased measurement as if it were a verdict.

## Non-goals

- Analysing the *rendered* output. These stats describe source pixels
  before tone- and gamut-mapping, per the existing `probe.h` contract.
- Dolby Vision / HDR10+ dynamic metadata.
- Any GPU involvement in `--analyze`.
- Realtime guarantees. **Explicitly not required** (user decision) —
  which is what lets `--analyze` run at stride 1.

## Prior art

### In this tree

`Decoder` parses `cll_max` / `cll_avg` (`src/decoder.h:35`) — the
file's *declared* MaxCLL / MaxFALL. Those are by definition accumulated
statistics, so the accumulator's headline numbers are the measured
counterparts. `diagnose.c` supplies the reporting idiom:
`PASS`/`WARN`/`FAIL`, a `summary:` footer, exit code = fail count
(`src/diagnose.c:236-237`).

### vca.py — overlap accepted

`../hdr-analysis/vca.py` (55KB) already does offline analysis:
`analyze_frame_luminance`, `classify_content`, `analyze_gamut_claimed`,
`check_all_mislabels`, plotting. `--analyze` duplicates part of this.

**This overlap was raised and accepted by the user**, on the grounds
that a single binary with an exit code is worth the duplication. Two
consequences to keep in mind:

- vca.py samples ~10 frames at timestamps (`vca.py:208`), so it *cannot*
  produce a true MaxCLL. Every-frame measurement is the one thing
  `--analyze` does that vca.py structurally cannot.
- Bug-fix parity is now a maintenance cost. Concretely, vca.py has the
  same class of HLG bug fixed here in C1: `analyze_frame_luminance`
  (`vca.py:242`) does `hlg_oetf_inverse(v) * 1000.0`, omitting the
  system gamma, which overstates shadows by up to ~2.5×. Its
  `classify_content` HLG branch (`vca.py:288`) correctly uses code
  values rather than nits, so the verdict is unaffected — only the
  reported numbers. **Tracked separately; not part of this work.**

### Not used

- **libplacebo peak detection** (`pl_peak_detect_params`) already
  computes a GPU histogram of PQ luma with scene-change detection. Not
  used because it is *target*-referenced (post-tone-map) and tied to
  render state, whereas these stats are deliberately source-referenced.
  It is also unavailable to headless `--analyze`.
- **ffmpeg `signalstats`** gives full-coverage SIMD YMIN/YAVG/YMAX. Not
  used because it does not do PQ/HLG decode to nits, does not produce
  the histogram, and would forfeit the single-binary UX.

## Why naive min/max fails when accumulated

Per-frame extremes are single-sample outliers — survivable per-frame,
fatal accumulated, because a running max/min is monotonic and latches.

- One ringing artifact in 10,000 frames pins session peak at 10000 nits.
- The floor is worse. `floor_cutoff_nits = 0.05` (`src/probe.c:200`)
  already exists because min is fragile. Over a whole file the session
  floor finds *some* pixel at ~0.051 nits, so "total DR" degenerates to
  a near-constant ~17.3 stops for every clip.

Percentiles are what make an accumulated dynamic range mean anything.

---

## 1. Luminance reference — the transfer functions are not commensurable

**This is C1, and it must be settled before any binning happens.**
`probe.c` currently selects an `eotf` and calls all three outputs
"nits" (`src/probe.c:180-185, 233`). They are not the same quantity:

| Transfer | Current output | Absolute? |
|---|---|---|
| PQ (`SMPTE2084`) | display nits, by definition | yes |
| HLG (`ARIB_STD_B67`) | **scene**-referred linear, 0..1 | **no** |
| SDR | `100 · V^2.4` | nominal only |

Left as-is, `--analyze` would hard-FAIL every HLG file (measured peak
≤ 1.0 trips "PQ/HLG but peak < 100N") with a nonzero exit code, while
the MaxCLL check *silently passes* (~1 vs declared 1000) — a file that
looks verified when nothing was verified.

### HLG: apply the OOTF

BT.2100's scene→display step, currently skipped:

```
gamma       = 1.2 + 0.42 * log10(L_W / 1000)
Y_display   = L_W * Y_scene^gamma
```

`hlg_inverse_oetf` already returns normalized scene linear — verified
to evaluate to exactly 1.0 at V=1.0 — so this composes directly onto
its output.

`L_W` is an assumption and must be explicit:

1. `mdcv_max_luma` when `has_mastering_display` (`src/decoder.h:33`).
2. Otherwise 1000 nits (BT.2100 reference).
3. `--hlg-peak N` overrides both.

The chosen `L_W` is stamped into HUD and `--analyze` output, so no HLG
number is ever readable without its premise.

### The reference tag

`FrameStats` and `SessionStats` carry:

```c
typedef enum {
    LUM_ABSOLUTE_PQ,     /* nits, absolute                    */
    LUM_HLG_OOTF,        /* nits, given a stated L_W          */
    LUM_SDR_RELATIVE,    /* nominal, 100-nit reference white  */
} LumReference;
```

The split that matters is **by kind of statistic**, not by transfer:

- **Ratio stats** — DR in stops, spread in stops, percentile ratios —
  valid for all three references. Units cancel.
- **Absolute stats** — MaxCLL, MaxFALL, "above 500N" — run only for
  `LUM_ABSOLUTE_PQ` and `LUM_HLG_OOTF`. For `LUM_SDR_RELATIVE` they are
  **suppressed, not caveated**: `sdr_eotf` caps at 100 nits by
  construction, so a measured MaxCLL for an SDR file is a tautology.

`stats.c` never inspects the tag. It only ever sees numbers plus a
reference, exactly as the user framed it: the fix belongs entirely in
the transform stage.

## 2. Data model: dual log-luma histograms

**Bins.** 512 bins of 1/16 stop spanning 2^-16 … 2^16 nits:

```
bin = clamp((log2(nits) + 16) * 16, 0, 511)      /* 32 stops * 16 */
```

1/16 stop = 4.4% luminance resolution, giving ~±0.06 stops of
quantization error on a DR figure printed to one decimal. (v1's 1/8
stop gave ±0.25 stops — worse than the printed precision.)

**Two histograms, not one** (C3):

- **luma** — drives DR, spread, percentiles. Perceptually the right
  axis, and cheap (luma plane only).
- **maxRGB** — `max(R,G,B)` per pixel, which is what CTA-861.3 actually
  defines MaxCLL and MaxFALL over. Requires full RGB reconstruction.

512 × 8 B × 2 = 8 KB session-side; 512 × 4 B × 2 = 4 KB per frame.

`probe_frame_stats` gains a mode: **luma-only** (fast, live HUD) or
**full-RGB** (exact, `--analyze`). In luma-only mode the maxRGB
histogram is absent and MaxCLL/MaxFALL are reported as unavailable
rather than approximated.

### Black, underflow, and the floor (C5)

v1 claimed a zero-counter retires `floor_cutoff_nits`. It does not.
At 10-bit limited range, `y_raw = 64` → `Y = 0` → caught; but
`y_raw = 65` → `pq_eotf(0.00114) ≈ 1e-7` nits → clamps into **bin 0**.
Real encodes have dithered bars and shadow noise across `y_raw` 63-70,
so bin 0 fills with thousands of pixels at a nominal 2^-16 nits, and
`log2(peak / bin0)` gives ~26 stops — the same degenerate constant, one
bucket over.

Three separate populations:

- **black** — `y_raw <= y_lo` (code domain, exact, depth-aware). Its own
  counter. Reported as `BLACK 12PCT`; letterbox detection falls out.
- **underflow** — landed in clamp-bin 0. Counted separately, **excluded
  from the percentile population**, and reported if non-trivial.
- **valid** — everything else. Percentiles are computed over this.

The robust DR floor is **p1, not p0.1**. The bottom 0.1% of a dark HDR
frame is codec noise numbering in the thousands, not a handful of
outliers; p0.1 does not escape it. p99.9 is kept for the ceiling, where
the highlight population genuinely is sparse.

```
DR_robust = log2(p99.9 / p1)
```

### The lookup table (S2)

`eotf(Y)` is a pure function of the integer `y_raw` — confirmed:
`max_raw`/`y_lo`/`y_hi` derive only from depth (`src/probe.c:175-177`),
`full_range` only from `color_range`, the clamp is deterministic, and
the selector only from `color_trc`. So a `y_raw -> {float nits, uint16
bin}` LUT is valid, rebuilt when depth / range / `color_trc` / `L_W`
changes. It absorbs the new OOTF `pow()` for free.

Sizing: `float` + `uint16` = 6 B × 4096 = 24 KB, chosen to stay near
L1d. (A `{double, int}` LUT would be 48 KB.)

**Scope and honest perf claims.** The LUT covers `probe_frame_stats`
only. `probe_sample` (`src/probe.c:144-146`) applies the eotf to
continuous per-channel RGB after the YUV→RGB matrix and cannot use it —
that is one pixel per frame, so it is irrelevant to throughput. The
refactor must not accidentally route `probe_sample` through it.

v1 called the existing `pow()` pair "very likely the most expensive
thing in the decode loop". That was unverified and is probably
overstated — ~130k samples × 2 `pow()` is a few ms/frame against 10-30
ms/frame for 4K HEVC 10-bit software decode, so plausibly 10-30% of
decode-loop CPU. The real justification is stride 1: 8.3M samples/frame
instead of 130k, a 64× increase that only a table lookup makes
tolerable.

**Benchmark method matters.** Playback FPS will not move — that path is
PTS-paced with a `nanosleep` (`src/main.c:550`). Measure `--analyze`
wall-clock throughput, or a standalone microbenchmark. Nothing else is
a falsifiable gate.

## 3. The accumulator

New `src/stats.c` / `stats.h`.

```c
typedef struct {
    uint64_t hist_luma[HIST_BINS];    /* session, bin-wise sum      */
    uint64_t hist_maxrgb[HIST_BINS];  /* full-RGB mode only         */
    uint64_t black, underflow, valid_samples;

    double   maxcll_nits;     /* max over frames of frame maxRGB max */
    double   maxfall_nits;    /* max over frames of frame maxRGB avg */
    double   min_frame_avg;

    uint64_t frames;
    double   mu_mean, mu_m2;      /* Welford over per-frame log2 means */
    double   within_var_sum;      /* Sum_f n_f * Var_within(f)         */
    uint64_t within_n_sum;        /* Sum_f n_f                         */

    int64_t  high_water_pts;      /* dedupe key                        */
    int64_t  start_pts, duration; /* for coverage                      */

    LumReference reference;
    double       hlg_lw;
} SessionStats;

bool session_stats_add(SessionStats *s, int64_t pts, const FrameStats *f);
```

### Dedupe: PTS high-water mark (C4)

v1 keyed dedupe on `current_frame_no` + a bitset. That computation
(`src/main.c:520-532`) is `round(sec * avg_frame_rate)`, which collides
and skips on VFR, double-counts on its no-PTS fallback branch (a plain
decode counter that keeps incrementing across seeks), goes negative on
B-frame reorder, and breaks on non-zero container `start_time`. All
four fail silently.

Replaced with a high-water mark. Playback is monotonic between seeks:

```c
if (pts > s->high_water_pts) {
    accumulate(s, f);
    s->high_water_pts = pts;
}
coverage = (double)(high_water_pts - start_pts) / duration;
```

Exact. No `nb_frames`, no fps, no bitset, no fallback chain. Works on
VFR. Cannot collide, cannot double-count. It *under*-counts — frames
between a backward seek and the high-water are skipped even if unseen —
but it never silently corrupts, which is the property a verdict needs.

Coverage comes from `AVFormatContext::duration`. If duration is unknown,
coverage is `-1` and the absolute verdicts stay gated shut.

### Spread, fully specified (C6)

v1 mixed linear-nits `avg_nits` with log2-domain spreads, so the law of
total variance would not have held and the self-test would have failed.
All three terms are now log2-domain over the **valid** population
(excluding black and underflow):

- `mu_f` = mean of `log2(nits)` over frame *f*'s valid samples, derived
  from that frame's histogram. **New field in `FrameStats`.**
- `Var_within(f)` = log2-domain variance within frame *f*, same source.
  **New field in `FrameStats`** — this is what feeds `within_var_sum`,
  which v1 left with no source at all.
- `temporal² = Var_f[mu_f]`, weighted by `n_f` (matters if resolution
  changes mid-stream).
- `spatial² = Σ_f n_f · Var_within(f) / Σ_f n_f`.
- `total²` = pooled log2 variance recovered from the session histogram.

Then `total² == spatial² + temporal²` genuinely holds and is testable.

Note `mu_f` is a *different number* from `avg_nits`. Both exist:
`avg_nits` is linear-domain over all samples and feeds MaxFALL;
`mu_f` is log2-domain over valid samples and feeds the spreads.

### Mid-stream format change (S4)

`README.md:179` notes there is no recovery from one. The accumulator
would silently pool histograms across different resolutions, depths or
transfers. On any change to `width`/`height`/`format`/`color_trc`/
`color_range`: hard-reset the accumulator and log a warning.

## 4. HUD surfacing

New `SLOT_SESSION`, anchored **bottom-right** — verified free in all
modes (`src/hud.c:390-406`).

### Compositing (C2) — v1 was wrong about this

v1 quoted `src/hud.c:366-374` describing two render passes and proposed
attaching the panel to both. **That comment is stale.** `renderer.c`
does exactly one `pl_render_image` into the swapchain at full crop
(`src/renderer.c:729, 749`); the SDR half is an offscreen `diag_tex`
(`:475`) composited as an overlay, with all HUD overlays on that single
render (`:738-748`).

So:

- **One** overlay entry. No per-orientation special-casing; the whole
  v1 "split-mode wiring" section is deleted.
- Appended **after** `sdr_ov`. Ordering is load-bearing: the SDR overlay
  is alpha=1 in its visible region and erases anything composited before
  it (`src/renderer.c:732-737`).
- `ov_arr` is `struct pl_overlay ov_arr[4]` (`:738`) and already fills
  all 4 slots in SPLIT mode. Grow to `[6]`. Neither the compiler nor
  `-Wall -Wextra` would catch this overflow.
- Attaching it twice, as v1 proposed, would double-blend alpha 170 to
  ~0.89 and overflow the array. Do not.

**Also fix the stale comment at `src/hud.c:366-374`** — it caused this
error and will cause the next one.

### Keys

Taken: `ESC/Q F H S P O SPACE L R <-/-> M I` (`src/main.c:422-483`).

- `A` — toggle the accumulated panel (default off).
- `Shift+A` — reset the accumulator.

Every existing handler tests `e.key.key` with no modifier check, so
`SDLK_A` matches regardless of shift. Both branches must test
`e.key.mod & SDL_KMOD_SHIFT` explicitly and be mutually exclusive,
or `Shift+A` will toggle *and* reset (S7).

`R` (restart) deliberately does not reset — with the high-water mark,
replayed frames are skipped anyway.

### Layout

440 × 200, seven lines at the existing 24 px pitch.

```
SESSION 842F  COVER 66PCT
MaxCLL  MIN 4210N  DECL 1000N     <- red only when measured > declared
MaxFALL MIN  312N  DECL  280N
P50 48N   P1 2N   P99 890N
DR 11.4 STOPS (P99.9/P1)
SPREAD 2.1 SPAT / 0.9 TEMP
BLACK 12PCT  OUT709 8PCT
```

`FONT[]` (`src/hud.c:30-76`) has no `>`, `<`, `%` or `,` — `find_glyph`
returns NULL and `draw_text_color` renders a solid block
(`src/hud.c:95`). So: add a `>` glyph, or use `MIN` as above. `PCT`
already matches existing style.

In luma-only mode (the live path) the MaxCLL/MaxFALL lines read
`LUMA-ONLY` rather than a number, since luma is not maxRGB.

### Fold in the status-panel truncation (S11)

Confirmed: pitch 24, first line at `y=6`, so line 9 (`y=198`) is the
last that fits in `H=220`. In SPLIT mode with the probe active the panel
draws 12 lines — **`SDR BOOST` and both `PROBE` lines are invisible
today**, i.e. pressing `M` in split mode appears to do nothing. One
token: `H = 220` → `340` (`src/hud.c:204`). Worth taking while editing
`hud.c`.

## 5. `--analyze`

New `src/analyze.c`. First lift `check()` / counters out of
`diagnose.c` (`src/diagnose.c:19-32`) into a shared `checks.h`, adding
an `isatty()` guard — it currently emits ANSI unconditionally, which
breaks when piped (S9).

```c
int analyze_run(const char *path, int stride, double hlg_lw, bool json);
```

Decodes every frame to EOF in **full-RGB mode at stride 1** by default
(`--stride N` to override). No SDL, no Vulkan, no window.

**Dispatch before video init** (S12): the `--analyze` branch must return
before `src/main.c:362-363`, which calls `renderer_list_displays()` and
thus `SDL_Init(SDL_INIT_VIDEO)` (`src/renderer.c:93-101`). Note
`ensure_moltenvk_icd()` at `:340` runs first regardless and will log ICD
lines into the output on macOS.

**Per-frame metadata.** `decoder.c` reads only *stream*-level side data
(`src/decoder.c:48-92`), though the comment at `:46-47` promises
per-frame upgrades. Since `--analyze` walks every frame anyway, read
`AV_FRAME_DATA_CONTENT_LIGHT_LEVEL` and
`AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` there — otherwise files
carrying CLL only per-frame get no comparison at all.

### Checks

| Check | Rule |
|---|---|
| coverage | PASS only on clean EOF |
| **unsupported pix_fmt** | distinct exit path — not a FAIL |
| **illegal range** | FAIL if a limited-range-tagged file carries significant `y_raw < y_lo` or `> y_hi` |
| PQ/HLG but peak < 100N | FAIL — SDR mislabeled in an HDR container. *Absolute-reference only* |
| MaxCLL vs declared | FAIL if `measured > declared`; else **INFO / not disproven** |
| MaxFALL vs declared | same, with the letterbox correction below |
| `mdcv_max_luma` vs peak | WARN if pixels exceed the mastering-display claim |
| above 500N | WARN if < 0.01% file-wide. *Absolute-reference only* |
| out of BT.709 | WARN if a wide-gamut-tagged file has < ~1% of pixels outside 709 |
| robust DR | WARN below ~6 stops |
| black %, underflow % | informational |

Checks marked *absolute-reference only* are skipped for
`LUM_SDR_RELATIVE`. MaxCLL/MaxFALL are skipped entirely when
`has_cll == false`, and the mdcv check when
`has_mastering_display == false`.

### One-sided inference (C3)

Three biases sit between measurement and true MaxCLL, and all three push
the estimate **down**:

1. **Stride** — mitigated to zero at stride 1, but present in the HUD.
2. **Jensen** — `probe_frame_stats` decodes Y' directly
   (`src/probe.c:154-162`); the matrix is applied to *nonlinear* R'G'B',
   and all three EOTFs are convex on [0,1], so the result is ≤ true
   luminance. Not removable without full per-channel linearization.
3. **luma vs maxRGB** — removed by the maxRGB histogram.

Because the bias is one-directional, `measured > declared` is valid
evidence of under-declaration and the FAIL branch is sound. But
`measured <= declared` is **evidence of nothing**, so v1's `PASS` label
was wrong — a file under-declaring by 3× would print `PASS`. It becomes
`INFO`, with the detail line stating the bound and its basis:
`>= 4210N (stride 1, maxRGB, Jensen lower bound)`.

The `× 1.5` margin is dropped: a lower bound exceeding a claimed maximum
is a contradiction at any margin.

**MaxFALL and letterbox.** `avg_nits` (`src/probe.c:234`) divides by all
samples including bars. For 2.39:1 in a 16:9 container that is ~25% dead
area, depressing measured MaxFALL ~25% against a declared
(active-picture) value — systematically low on essentially every scope
film. BLACK% is already computed: exclude detected bar rows/columns from
the MaxFALL denominator, or at minimum print the correction factor.

### `--stats-file` — hdrplay measures, vca.py plots

The overlap with vca.py resolves into a pipeline rather than a
duplication: `--analyze` is a fast C measurement engine, vca.py keeps
classification and plotting.

This is worth doing because vca.py's slowness is structural.
`pq_eotf` (`vca.py:115-152`) flattens to a Python list and loops per
pixel — the docstring explains why (numpy 2.x `np.power` returned values
~1e9 too small) — so the workaround costs roughly two orders of
magnitude. That is almost certainly why it samples only 10 frames.
hdrplay decodes every frame at stride 1 and, with the LUT, spends one
table lookup per sample.

```
hdrplay --analyze --stats-file clip.ndjson clip.mov
vca.py --from-stats clip.ndjson          # plot only, no decode
```

**Format: NDJSON.** One JSON object per line — a header line carrying
schema version, source metadata, luminance reference and `L_W`; then one
line per frame; then a trailer with the session summary and both 512-bin
histograms. Streams straight out of the decode loop with no buffering,
`pd.read_json(lines=True)` reads it directly, and it stays greppable.

**Per-frame records carry scalars only**, not histograms: `pts`,
`frame_no`, luma peak/avg, maxRGB peak/avg, `mu_f`, `var_within`,
`black_pct`, `underflow_pct`, `out709_pct`. Enough for every time-series
plot vca.py draws. Per-frame histograms are deliberately excluded — 512
bins × 172k frames for a 2h feature is not a file anyone wants. Session
histograms go in the trailer, once.

At ~120 B/frame that is ~20 MB for a 2h feature at 24fps, which is fine.

**Separately, vca.py can have the same LUT trick.** Its PQ input is
quantized to 16-bit, so a one-off 65536-entry table turns
`analyze_frame_luminance` into `LUT[raw]` — pure numpy fancy indexing,
and it sidesteps the `np.power` bug entirely rather than working around
it. That would make vca.py fast enough to stop sampling. Tracked
separately from this work.

### Output and exit codes (S9)

- Checks and summary → **stderr** (matching `diagnose.c:29`).
- `--json` → **stdout**, or `| jq` breaks. Includes all 512 bins of both
  histograms, so two encodes of a master can be diffed.
- Exit code = fail count. But `main()` already returns 2 for usage
  errors (`src/main.c:329-358`) and 1 for `decoder_open` failure
  (`:366`) — indistinguishable from "2 FAILs" in a batch loop. Reserve
  **64+** for tool errors (unreadable file, unsupported format, decode
  abort) and document it.
- `decoder_next_frame()` returning -1 mid-file (`src/decoder.c:146,154`)
  is incomplete coverage and must suppress the verdicts — §4's "linear
  decode means coverage is 100% by construction" was wrong (S8).

## 6. Pixel-format safety (C7)

`probe_frame_stats` gates only on `comp[0].depth ∈ {8,10,12}`
(`src/probe.c:170-173`), then reads `data[0]` as packed planar at that
depth (`:204-208`). It never checks `shift`, `step`, or planarity.

**P010 — named in `README.md:13` as an expected input** — has
`depth == 10` but `shift == 6`; the bits sit high in a 16-bit word.
`Y = y_raw / 1023` overflows ~64×, clamps to 1.0 (`:217`), and every
sample reads 10000 nits. Today that is a wrong HUD line; under
`--analyze` it becomes `FAIL MaxCLL 1000N vs 10000N` for the whole
file, marking every hardware-decoded clip broken.

Add an explicit guard — `shift == 0`, planar, `step == 1` — and return
false otherwise. Then handle zero valid frames end-to-end: `--analyze`
must not print `PASS coverage 0/0` or divide by zero. It needs a
distinct "unsupported pixel format, nothing measured" path with a
tool-error exit code, not a FAIL.

Separately, `yuv422p10le` / `yuv444p10le` (ProRes-style masters, likely
inputs) work correctly in `probe_frame_stats` because it is
linesize-driven — but `probe_sample` hardcodes 4:2:0 chroma
(`src/probe.c:93`). The maxRGB path needs chroma, so **it must handle
4:2:2 and 4:4:4 subsampling** rather than reusing that assumption.

## 7. Testing

No test infrastructure exists. `CMakeLists.txt` has no
`enable_testing()`, no `include(CTest)`, and no library target —
`src/*.c` compile straight into the executable (`CMakeLists.txt:31-39`).
And `pq_eotf` / `hlg_inverse_oetf` / `sdr_eotf` are all `static`
(`src/probe.c:11,28,38`).

So step 1 includes: add an OBJECT library target for the non-`main`
sources, `enable_testing()`, link `m` on Linux, and either de-`static`
the EOTFs behind a header or have the test `#include "probe.c"`.
Prefer the OBJECT lib + a small internal header.

Properties to assert:

- **LUT equivalence, exhaustively.** For every `y_raw` across all
  depth × range × trc combinations: `(256 + 1024 + 4096) × 2 × 3 =
  32,256` comparisons to ~1e-9.
- **HLG OOTF.** `Y_S = 1 → L_W`; `L_W=1000, Y_S=0.1 → 63.1` not 100.
- **Percentiles vs brute force**, within one bin width.
- **`total² == spatial² + temporal²`** — now that all terms share a
  domain and population, this genuinely holds.
- **High-water dedupe:** replaying a PTS range adds nothing; a backward
  seek then forward replay leaves `frames` unchanged.
- **P010 and other shifted formats are rejected**, not misread.
- **Degenerate inputs:** all-black clip (empty valid population must not
  divide by zero), single sample, `p99.9 == p1` → DR 0 not NaN, unknown
  duration → coverage -1 and verdicts suppressed.

## Landing order

1. **Test scaffolding + LUT + OOTF + format guard.** `enable_testing()`,
   OBJECT lib, exhaustive equivalence test, C1 and C7 fixed. This is the
   correctness floor — C1 and C7 are live bugs in the HUD today,
   independent of anything accumulated. Benchmark via `--analyze`
   throughput or a microbenchmark, *not* playback FPS.
2. Dual histograms in `FrameStats` + `mu_f` / `Var_within` + percentile
   helpers + tests. Still invisible.
3. `stats.c` accumulator, high-water dedupe, coverage + tests.
4. HUD panel, `A` / `Shift+A`, `ov_arr[6]`, stale-comment fix, and the
   `H=340` truncation fix.
5. `checks.h` refactor, `analyze.c`, `--json`, exit-code reservation.
6. README.

## Decisions

| Decision | Alternatives rejected | Why |
|---|---|---|
| Dual log-luma + maxRGB histograms, 512 bins @ 1/16 stop | Single histogram; Welford stddevs; 256 bins @ 1/8 | Histogram makes accumulated extremes outlier-proof; maxRGB is what MaxCLL is defined over; 1/8 stop was coarser than the printed precision |
| PTS high-water mark | Frame-index bitset (v1) | Exact on VFR, no `nb_frames`/fps dependency, cannot silently corrupt; deletes the entire fallback chain |
| HLG OOTF with explicit `L_W` | Suppress all HLG absolute checks | Keeps HLG files measurable; the assumption is stated rather than hidden |
| Absolute checks suppressed for SDR | Print with a caveat | A measured MaxCLL ≤ 100N for an SDR file is a tautology, not a measurement |
| `INFO` not `PASS` on `measured <= declared` | Keep `PASS` | The measurement is a one-sided lower bound; `PASS` would green-light under-declaring files |
| DR floor at p1 | p0.1 | The bottom 0.1% of a dark HDR frame is codec noise in the thousands, not outliers |
| Live HUD *and* `--analyze` | Either alone | Same accumulator serves both |
| hdrplay measures, vca.py plots, via `--stats-file` | Move `--analyze` to vca.py; duplicate plotting in C | vca.py is ~100× slower than it needs to be (per-pixel Python loop in `pq_eotf`), which is why it samples 10 frames. C does exhaustive measurement, Python does matplotlib. Residual overlap in classification is accepted — cost is bug-fix parity, see the HLG note under Prior art |
| Gamut + illegal-range checks | Luminance only | Illegal-range catches full-range mistagging, which would invalidate every nit number; gamut is the second axis of HDR/SDR difference and `--sdr-gamut-map` exists because of it |
