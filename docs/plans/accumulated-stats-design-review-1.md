VERDICT: NEEDS_REVISION

## Summary Assessment

The data model (log-luma histogram, coverage-tracked accumulation, diagnose-style checks) is the right shape and most of the arithmetic checks out, but the design rests on three factually wrong premises about the codebase and the colour math — a stale comment in `hud.c` about split-mode render passes, the assumption that `eotf()` output is absolute nits for HLG, and the assumption that a strided Y'-derived peak is comparable to a declared MaxCLL. As specified, `--analyze` would emit a hard FAIL with a nonzero exit code on every HLG file and on every P010 file, which is worse than shipping nothing.

---

## Critical Issues (must fix)

### C1. The HLG path makes `--analyze` emit false FAILs on every HLG file

`hlg_inverse_oetf()` (`src/probe.c:28-35`) returns **scene-referred normalized linear in [0,1]**, exactly as its own comment says (`src/probe.c:26-27`). `V=1` maps to `(exp((1-c)/a)+b)/12 = 1.0`, not to a nit value. `probe_frame_stats()` then stores that into `out->peak_nits` (`src/probe.c:233`) as if it were cd/m².

The design bins that value on an absolute log2-nits scale and runs three absolute checks against it. For any HLG file:

- **"PQ/HLG but peak < 100N → FAIL — SDR content mislabeled in an HDR container"** fires unconditionally. Measured peak is ≤ 1.0 by construction, so a perfectly-graded HLG master gets a hard FAIL and a nonzero exit code. In the design's own batch loop (`for f in *.mov; do hdrplay --analyze "$f" || echo "$f suspect"; done`) every HLG clip is flagged suspect.
- **"above 500N < 0.01% → WARN"** fires unconditionally.
- **MaxCLL vs declared** silently PASSes (measured ~1 vs declared 1000), which is the *most* dangerous outcome: the file looks verified when nothing was verified.

The whole histogram is also useless for HLG — every sample lands in bins for 2^-16..2^0, i.e. the bottom 128 bins.

This is latent in the HUD today (`ABOVE 500N` and `SRC PEAK` already misread for HLG), but the design promotes it to a file verdict with an exit code. Fix one of:
- Apply the BT.2100 OOTF: `display_nits = Lw · Y_s^(γ-1) · rgb_s` with a nominal `Lw` (1000) and `γ = 1.2 + 0.42·log10(Lw/1000)`. Note this is a *per-frame-luma* operation, not a per-sample one, and it needs a declared `Lw` (take `mdcv_max_luma` when present, `src/decoder.h:33`).
- Or explicitly suppress all absolute-nits checks for `AVCOL_TRC_ARIB_STD_B67` and report relative (%-of-peak) numbers only.

Either way the design must state which, because the checks table currently assumes the first without doing the work.

Same class of problem, less severe, for SDR: `sdr_eotf()` is `100·V^2.4` (`src/probe.c:41`), so "measured MaxCLL" for an SDR file is ≤ 100 nits *by construction* and carries zero information. The design should say non-PQ sources get the absolute checks suppressed rather than leaving them to produce vacuous WARNs.

### C2. "Attach the session overlay to both split passes" is based on a stale comment; there are no split passes

The design quotes `src/hud.c:366-374` ("overlays are clipped to each render's `target.crop`... attach status + hdr_label to the FIRST (HDR-half) render; attach sdr_label to the SECOND"). **That comment no longer describes the code.** `renderer.c` was reworked to a single-swapchain-render architecture:

- The SDR half is rendered into the offscreen `diag_tex` (`src/renderer.c:475`), not into a half of the swapchain.
- There is exactly **one** `pl_render_image` into the swapchain for SDR and SPLIT modes (`src/renderer.c:749`), with `target = base_target` — the *full* window crop (`src/renderer.c:729`).
- All HUD overlays — status, hdr_label, sdr_label — are attached to that single render (`src/renderer.c:738-748`).

Consequences:

1. The stated problem ("a panel attached only to the first pass is invisible if it sits in the second pass's half") does not exist. A bottom-right session panel needs **one** overlay entry, no per-orientation special-casing. The design's whole "Split-mode wiring" section is solving a phantom.
2. The proposed remedy is an actual bug. Putting the same `pl_overlay` twice into the same `target.overlays` array composites it twice. `make_panel()` uses alpha 170 (`src/hud.c:207`), so the background would blend to `1-(1-170/255)² ≈ 0.89` instead of `0.667` — a visibly darker panel than every other HUD element. The design asserts "each clips to its own crop, so a panel straddling the seam renders correctly" — with one pass and one crop, that is just a double blend.
3. `ov_arr` is declared `struct pl_overlay ov_arr[4]` (`src/renderer.c:738`) and already fills all 4 slots in SPLIT mode. Adding a session overlay needs `[5]`; adding it twice needs `[6]`. Neither the compiler nor `-Wall -Wextra` (`CMakeLists.txt:12`) will catch the overflow.
4. Ordering is load-bearing and unmentioned: the SDR overlay is composited **first** and is opaque (alpha=1) in its visible region, so anything drawn before it is erased (`src/renderer.c:731-737`). The session panel must be appended *after* `sdr_ov`.

Also fix the stale comment at `src/hud.c:366-374` while you're there — it is what misled this design and will mislead the next one.

### C3. The MaxCLL comparison is not sound in the PASS direction, and the design's headline claim rests on it

The design calls "File claims MaxCLL 1000, pixels measure 4200" the single most useful thing the feature can say. Three independent biases sit between `probe_frame_stats` and MaxCLL:

1. **Stride.** At stride 8 (`src/renderer.c:630`) you inspect 1/64 of pixels. Specular glints and small emissive sources — precisely what sets MaxCLL — are the features most likely to fall between samples.
2. **Luma, not maxRGB.** MaxCLL is defined (CTA-861.3) as the max over pixels of `max(R,G,B)`, a per-component quantity. `probe_frame_stats` computes a luma. Since `kr+kg+kb = 1`, luma ≤ max(R,G,B) always, and for saturated highlights the gap is large. Same for MaxFALL, which is the max frame-average of maxRGB, not of luma. The design's definitions at lines 39-41 gloss this.
3. **Y' shortcut.** `probe_frame_stats` PQ-decodes Y' directly (acknowledged at `src/probe.c:154-162`). For BT.2020 NCL the matrix is applied to *nonlinear* R'G'B', so `EOTF(Σkᵢ Rᵢ') ≠ Σkᵢ EOTF(Rᵢ')`. All three EOTFs here are convex on [0,1] (`pq_eotf`, `100·V^2.4`, both HLG branches), so by Jensen the computed value is **≤** the true luminance.

All three biases point the same way, which is actually the design's saving grace: the measured value is a genuine **lower bound** on true MaxCLL. That makes `measured > declared` valid one-sided evidence of under-declaration — the FAIL branch is defensible.

But `measured ≤ declared` is **not** evidence of anything, and the design labels it `PASS`. A file that genuinely under-declares MaxCLL by 3× will routinely print `PASS MaxCLL declared`. A tool whose stated purpose is "is this file worth using as an HDR test clip?" that confidently green-lights broken files is worse than one that says nothing. Required changes:

- Rename the branch. `PASS` → `INFO` / `not disproven`, with the detail line spelling out `>= 4210N (stride 4, luma lower bound)`.
- The `× 1.5` FAIL margin then has no justification and should be dropped to a plain `measured > declared` (a lower bound exceeding a claimed maximum is a contradiction at any margin). If you keep a margin, state what it is protecting against.
- The `--analyze` sample output prints `1000N vs 4210N measured` with no `>=`, unlike the HUD mock. Make them consistent.
- Skip the check entirely when `has_cll == false` (`src/decoder.h:34`) and the mdcv check when `has_mastering_display == false`. The table doesn't say.
- `decoder.c` only reads *stream*-level side data (`src/decoder.c:48-92`); the comment at `src/decoder.c:46-47` promises "per-frame upgrades silently in the render path" but nothing calls `av_frame_get_side_data` anywhere in the tree. `--analyze` is already walking every frame — read `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL` / `AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` there, or files that carry CLL only per-frame get no comparison at all.

Related, and separately wrong: **MaxFALL vs letterbox.** The design tracks BLACK% but only reports it. `avg_nits` (`src/probe.c:234`) divides by all samples including letterbox bars. For 2.39:1 content in a 16:9 container that is ~25% dead area, depressing measured MaxFALL by ~25% relative to the (active-picture) declared value. The MaxFALL check as specified will read systematically low on essentially every scope-ratio film. If BLACK% is being computed anyway, use it: exclude black-bar rows/columns from the MaxFALL denominator, or at minimum print the correction factor.

### C4. `current_frame_no` is only a safe dedupe key for CFR content with sane PTS

The design says "`r->current_frame_no` already exists (`src/hud.c:253`), so the input is available." `hud.c:253` is a read site; the computation is `src/main.c:520-532`:

```c
if (stream_fps > 0.0 && pts != AV_NOPTS_VALUE) {
    double sec = (double)pts * av_q2d(stream_tb);
    rend.current_frame_no = (int)(sec * stream_fps + 0.5);
} else {
    rend.current_frame_no = (rend.current_frame_no < 0 ? 0 : rend.current_frame_no + 1);
}
```

with `stream_fps` from `avg_frame_rate` (`src/main.c:412-413`). Four failure modes, all silent:

- **VFR.** `avg_frame_rate` is an average. `round(sec × fps)` will produce **collisions** (two distinct frames → one index) and **skips**. A collision makes `session_stats_add` return false and silently discard a real frame, biasing the histogram. A skip means frame coverage never reaches 100%, so the coverage gate that guards the MaxCLL verdict never opens on VFR content in live mode.
- **The fallback branch.** When fps or PTS is unavailable the index is a plain decode counter that keeps incrementing across seeks. `renderer.h:117-119` claims it "stays correct across seeks" — true only for the PTS branch. On the counter branch, re-watched frames get *new* indices and dedupe silently double-counts, which is precisely the failure the design says dedupe exists to prevent. Dedupe must be disabled (and coverage forced to -1) whenever this branch is taken.
- **Non-zero container start time.** MPEG-TS wall-clock PTS (and MOV edit lists) make `sec` large. `nb_frames` would then be tiny relative to the index, so the "grow the bitset" fallback allocates for the wrong denominator and coverage reads ~0% forever. Subtract `AVStream::start_time`.
- **Negative PTS.** B-frame reorder / edit-list offsets can produce a negative `best_effort_timestamp`; `(int)(sec*fps + 0.5)` then yields a negative index. Fallback #2 covers "out of range high", not negative — that's an out-of-bounds bitset write.

Concretely: state the precondition, subtract `start_time`, reject negative indices, and disable dedupe on the counter branch. See S1 for a simpler key that sidesteps all of this.

### C5. "Pure black gets its own counter... this retires the `floor_cutoff_nits` hack properly" is false

`floor_cutoff_nits = 0.05` (`src/probe.c:200`) exists because near-black codes dominate the minimum. Giving *exactly zero* its own counter does not fix that:

- Limited range, 10-bit: `y_lo = 64`. `y_raw = 64` → `Y = 0` → nits 0 → caught by the black counter. But `y_raw = 65` → `Y = 1/876` → `pq_eotf(0.00114) ≈ 1e-7` nits. Not zero, so it goes into a bin — and `clamp((log2(1e-7)+16)*8, 0, 255)` = 0, i.e. bin 0.
- Real encodes have dithered/noisy letterbox bars and shadow noise spanning `y_raw` 63-70. Sub-`y_lo` codes are clamped to `Y=0` (`src/probe.c:216`) so they land in black; codes just above pile into bin 0.
- Bin 0 is a clamp bucket, so its nominal value is 2^-16 = 1.5e-5 nits. A p0.1 that lands in bin 0 yields `DR = log2(peak / 1.5e-5) ≈ 26+ stops` — the same degenerate-constant failure the design correctly diagnoses for the naive min, just relocated one bucket over.

The bottom 0.1% of a dark HDR frame is not a handful of outliers; it is thousands of codec-noise pixels. p0.1 does not solve this. Either define "black" in the **code domain** (`y_raw <= y_lo`, exact and depth-aware) and report a separate sub-black/underflow count for bin 0 that is excluded from percentiles, or move the floor percentile up (p1 or p5) and say why. As written, the DR readout is the same near-constant number the design set out to eliminate.

### C6. The variance decomposition as specified cannot hold, and its self-test will fail

`SessionStats` mixes two domains:

- `maxfall_nits` / `min_frame_avg` are fed from `FrameStats::avg_nits`, which is a **linear-nits** mean over **all** samples including black (`src/probe.c:234`).
- `avg_mean` / `avg_m2` are described as "Welford over per-frame means", and the spreads are stated to be "both computed in log2-nits (stops)".

`total² = spatial² + temporal²` (law of total variance) requires the frame means and the pooled total to be in the *same* domain over the *same* sample set. A linear-domain mean over all samples is not the mean of log2(nits) over non-black samples. The identity will not hold and the §5 test ("the variance decomposition checks itself") will fail — which is bad, because the design presents that test as the correctness guarantee.

Also, `within_var_sum` has **no source**. `FrameStats` does not compute a per-frame variance and the design never adds one. You can derive it from the per-frame histogram, but the design has to say so, and it has to be the log2-domain variance over the same non-black population as the temporal term. Specify:

- `μ_f` = mean of log2(nits) over the frame's non-black samples, derived from `hist`.
- `temporal² = Var_f[μ_f]` weighted by per-frame sample count (needed if resolution can change).
- `spatial² = Σ_f n_f · Var_within(f) / Σ_f n_f`.
- `total²` = the pooled log2-domain variance recoverable from the session histogram.

And note this frame mean is a *different number* from `avg_nits` / MaxFALL; both need to exist.

### C7. `probe_frame_stats` accepts pixel formats it cannot read, and `--analyze` turns that into a false FAIL

`probe_frame_stats` gates only on `desc->comp[0].depth ∈ {8,10,12}` (`src/probe.c:170-173`), then reads `data[0]` as a packed planar array of that depth (`src/probe.c:204-208`). It never checks `desc->comp[0].shift`, `desc->comp[0].step`, or planarity. `probe_sample` at least documents and enforces a narrow format list (`src/probe.h:20-22`, `src/probe.c:88`); `probe_frame_stats` does not.

P010 — which `README.md:13` explicitly names as an expected input — has `comp[0].depth == 10` but `shift == 6`: the 10 bits sit in the *high* bits of a 16-bit word. `Y = y_raw / 1023` then overflows by ~64×, clamps to 1.0 (`src/probe.c:217`), and **every sample reads 10000 nits**. Today that is a wrong HUD line. Under `--analyze` it becomes `FAIL MaxCLL declared 1000N vs 10000N measured` plus a nonzero exit code, for the entire file. A batch triage run would mark every hardware-decoded clip as broken.

Add an explicit format guard (`shift == 0`, planar, `step == 1`) and return false otherwise. Then handle "zero valid frames" end-to-end: `--analyze` must not print `PASS coverage 0/0` or divide by zero — it needs a distinct "unsupported pixel format, nothing measured" exit path. The §5 degenerate-input list covers an all-black clip but not a zero-stats clip.

(Note: `yuv422p10le` / `yuv444p10le` — ProRes-style HDR masters, very likely inputs for this workflow — *do* work correctly in `probe_frame_stats` since it is linesize-driven. But `probe_sample` hardcodes 4:2:0 chroma (`src/probe.c:93`) and would be wrong for them, which matters if C3's fix pushes you toward per-channel maxRGB.)

---

## Suggestions (nice to have)

**S1. A PTS high-water mark is a much simpler dedupe key than a frame-index bitset.** Playback is monotonic between seeks. Track `int64_t high_water_pts` and accumulate only when `pts > high_water`. That is exact, needs no `nb_frames`, no fps, no bitset, no fallback chain, works on VFR, cannot collide, and cannot double-count. It under-counts (frames between a backward seek and the high-water are skipped even if unseen) but never *silently corrupts*, which is the property that matters for a verdict. Coverage becomes `(high_water - start) / duration`, available from `AVFormatContext::duration` without `nb_frames`. This eliminates C4 entirely and removes ~40% of the moving parts in §2. Worth putting back to the user since coverage-tracked dedupe was their explicit choice — the mechanism can change without changing the guarantee.

**S2. The LUT is real but the perf story is unverified and the step-1 gate is unfalsifiable as written.** The purity claim checks out: in `probe_frame_stats`, `nits` depends only on `(y_raw, depth, full_range, color_trc)` — `max_raw`/`y_lo`/`y_hi` are functions of depth (`src/probe.c:175-178`), the clamp is deterministic, and the eotf selector is a function of `color_trc` (`src/probe.c:180-185`). A 4096-entry LUT is valid.

But: (a) `probe_frame_stats` is called once per decoded frame on the playback path (`src/renderer.c:630`), and that path is **PTS-paced with a `nanosleep`** (`src/main.c:550`) — playback FPS will not move at all, so "benchmark the LUT before proceeding" has no observable metric unless you write a microbenchmark or measure `--analyze` throughput. Say which. (b) 129,600 samples × 2 `pow()` is on the order of a few ms/frame, against 4K HEVC 10-bit software decode at 10-30 ms/frame — plausibly 10-30% of decode-loop CPU, not "very likely the most expensive thing in the decode loop today". Soften the claim or measure it. (c) A `{double nits, int bin}` LUT at 12-bit is 4096 × 12B = 48 KB, over typical L1d. Use `float` + `uint8_t` (20 KB). (d) The LUT does **not** cover `probe_sample` (`src/probe.c:144-146`), which applies the eotf to continuous per-channel R/G/B after the YUV→RGB matrix — but that's one pixel per frame, so it's irrelevant to perf. Just say so, and make sure the refactor doesn't accidentally route `probe_sample` through a luma LUT. (e) The exhaustive test is `(256+1024+4096) × 2 ranges × 3 trcs = 32,256` comparisons, not "~25k".

**S3. Prior art the design should engage with rather than ignore.**
- **libplacebo's own peak detection.** `pl_peak_detect_params` (used via `pl_color_map_params`) already computes a per-frame GPU histogram of PQ-encoded luma with a configurable `percentile` field and scene-change detection, and `pl_renderer_get_hdr_metadata` exposes the result. For the *live HUD* case the GPU is already computing most of this as part of tone-mapping. The `--analyze` non-goal ("no GPU involvement") justifies not using it headless, but the design should say why it isn't used for the live panel — the honest answers (it's target-referenced, not source-referenced, and it's tied to render state) are good ones and belong in the Decisions table.
- **ffmpeg.** `signalstats` gives per-frame YMIN/YLOW/YAVG/YHIGH/YMAX at full pixel coverage with SIMD, via `ffprobe -f lavfi -i "movie=x.mov,signalstats" -show_entries frame_tags`. That is a strictly better peak/floor measurement than a strided CPU loop. The argument for building this in-tree is the PQ/HLG decode, the histogram, and the single-binary UX — state it.
- **`vca.py`.** `README.md:4` and `src/decoder.h:18` both name it as the existing tool for "is this file really HDR?" / "do the pixels back the claim". The design never mentions it. That is the single biggest duplication risk in this plan, and the one the user is best placed to adjudicate. The design should have a paragraph on what `--analyze` does that `vca.py` doesn't (my guess: the live HUD is genuinely new, and single-binary batch triage with an exit code is genuinely new; the measurement itself may not be).

**S4. Domain signals an HDR reviewer would insist on that the checks table omits.**
- **Illegal-range detection.** `probe_frame_stats` clamps Y to [0,1] (`src/probe.c:216-217`), so sub-black and super-white codes are silently discarded. If a file tagged limited-range carries significant `y_raw < y_lo` or `> y_hi`, it is almost certainly full-range mistagged — which makes *every* nit number this tool prints wrong. Detecting it costs two counters in the existing loop and is arguably the highest-value check available. It belongs in the table.
- **Gamut.** "Is this a good HDR test clip?" is at least as much about wide gamut as peak nits — this tool's own SDR pane does gamut mapping and `--sdr-gamut-map` exists precisely because gamut is "a real second axis of HDR/SDR difference" (`src/renderer.c:455-460`, `src/main.c:261-272`). An analyzer that reports peak/DR/spread but says nothing about % of pixels outside BT.709 answers half the question. `SPREAD 2.1 SPAT / 0.9 TEMP` is a much weaker decision signal than "8% of pixels outside 709".
- **Mid-stream format change.** `README.md:179` says there's no recovery from it, and the accumulator would silently mix histograms across resolutions/depths/transfers with unequal weighting. Invalidate (or hard-reset with a logged warning) on any change to `width/height/format/color_trc/color_range`.
- **Scene-cut awareness.** Whole-file "temporal spread" conflates shot-to-shot grading variance with within-shot pans, and with dedupe + arbitrary seeking it's computed over an arbitrary subset in arbitrary order. Statistically fine, but gate it on high coverage the way MaxCLL is gated, or say it's indicative only.

**S5. Histogram resolution vs printed precision.** The bin math is correct (see Verified), but 1/8 stop = 0.125 stop quantization on each endpoint means `DR 11.4 STOPS` carries ±0.25 stops of bin error while printing one decimal. Either go to 1/16 stop (512 bins, still 4 KB session-side) or print to 0.5 stop. Separately, ~44 of the 256 bins are dead for PQ (bins 0-21 are below 0.0001 nits, bins 235-255 above 10000) — retargeting the range to 2^-14..2^14 at 1/16 stop would spend the same memory better.

**S6. The `>` glyph does not exist.** `FONT[]` (`src/hud.c:30-76`) has `=` but no `>`, `<`, `%`, or `,`; `find_glyph` returns NULL and `draw_text_color` renders `0x7E` — a solid block (`src/hud.c:95`). The mock's `MaxCLL  >=4210N` would render as `▮=4210N`. (The mock is correctly avoiding `%` with `BLACK 12PCT`, matching existing style.) Add a `>` glyph or use `MIN 4210N`.

**S7. `Shift+A` will also fire plain `A`.** Every existing key handler tests `e.key.key` with no modifier check (`src/main.c:422-483`). `SDLK_A` matches whether or not shift is held, so `Shift+A` would toggle *and* reset. The design must specify the `e.key.mod & SDL_KMOD_SHIFT` test and the mutual exclusion.

**S8. Coverage denominator needs to be self-consistent.** If `nb_frames` is nonzero but too small and fallback #2 grows the bitset, coverage can exceed 100% or read 100% early. Make the denominator `max(estimate, highest_index_seen + 1)` and mark it as an estimate in the HUD (`COVER ~66%`). Also: in `--analyze`, if `nb_frames` is unknown, coverage is trivially `seen/seen` — print "N frames, decoded to EOF" rather than a meaningless percentage. And handle `decoder_next_frame()` returning -1 mid-file (`src/decoder.c:146,154`): that's incomplete coverage and must suppress the verdict, which §4's "linear decode means coverage is 100% by construction" doesn't account for.

**S9. Output stream and exit-code collisions.** `diagnose.c`'s `check()` writes to **stderr** (`src/diagnose.c:29`), as does the summary (`src/diagnose.c:236`). `--json` must go to **stdout** or `| jq` breaks; say so. Also `main()` returns 2 for usage errors (`src/main.c:329,335,337,358`) and 1 for `decoder_open` failure (`src/main.c:366`) — indistinguishable from "2 FAILs" / "1 FAIL" in the batch loop. Pre-existing in `--diagnose`, but the design's headline use case is exactly that loop, so reserve a high exit code (e.g. 64+) for tool errors. Finally, the lifted `check()` should gain an `isatty()` guard now that its output may be piped — it currently emits ANSI unconditionally.

**S10. Test wiring is not as cheap as §5 implies.** `CMakeLists.txt` has no `enable_testing()`, no `include(CTest)`, and no library target — `src/*.c` compile straight into the `hdrplay` executable (`CMakeLists.txt:31-39`). A `tests/test_stats.c` needs a new OBJECT or STATIC lib target (or to `#include "probe.c"`), plus `m` on Linux. And the exhaustive LUT-equivalence test needs `pq_eotf` / `hlg_inverse_oetf` / `sdr_eotf`, all `static` in `probe.c` (`src/probe.c:11,28,38`). Say which approach; it's step 1 of the landing order.

**S11. The status-panel truncation is confirmed and worse than described.** Verified: `H = 220`, `hud_scale = 2`, pitch = `FONT_H*2 + 8 = 24`, first line at `y=6` (`src/hud.c:204,133,222`). Line *n* starts at `6 + 24(n-1)` and is 16 px tall, so line 9 (`y=198`) is the last that fits. In SPLIT mode with the probe active the panel draws MODE, DISPLAY HDR, src csp, out csp, FRAME, SRC PEAK, DR, SDR CAP, ABOVE 500N, SDR BOOST, PROBE, RGB — 12 lines. **`SDR BOOST` and both `PROBE` lines are invisible today**, i.e. pressing `M` in split mode does nothing visible. The design is right that it's out of scope, but it's a one-token fix (`H = 220` → `340`) and it's a strictly worse bug than the one this feature addresses; consider folding it in since you're editing `hud.c` anyway.

**S12. Dispatch site, not just parse site.** The design cites `src/main.c:310` for the flag. The site that matters for "no SDL, no Vulkan, works over SSH" is the dispatch block at `src/main.c:346-358` — the `--analyze` branch must return *before* `src/main.c:362-363`, which calls `renderer_list_displays()` and thus `SDL_Init(SDL_INIT_VIDEO)` (`src/renderer.c:93-101`). Note `ensure_moltenvk_icd()` at `src/main.c:340` runs first regardless; harmless, but it will log ICD-discovery lines into `--analyze` output on macOS.

---

## Verified Claims

These I checked against the source and they are correct:

- **Bin arithmetic is right.** 2^-16..2^16 is 32 stops; at 1/8 stop that is exactly 32×8 = 256 bins. `clamp((log2(nits)+16)*8, 0, 255)` maps 2^-16→0 and saturates at 255. `2^(1/8) = 1.0905`, so "~9% luminance resolution" is accurate. The range does cover PQ's 0.0001–10000 nits (2^-16 = 1.5e-5; 2^16 = 65536).
- **`eotf(Y)` is a pure function of the integer `y_raw` in `probe_frame_stats`.** `max_raw`, `y_lo`, `y_hi` derive only from depth (`src/probe.c:175-177`), `full_range` only from `color_range` (`src/probe.c:178`), the clamp is deterministic (`src/probe.c:216-217`), and the eotf selector only from `color_trc` (`src/probe.c:180-185`). The LUT is sound, and 4096 entries covers 12-bit.
- **PQ costs two `pow()` per sample.** `pow(V, 1/m2)` and `pow(num/den, 1/m1)` (`src/probe.c:19,23`).
- **`probe_frame_stats` is on the per-frame hot path.** Called unconditionally at stride 8 for every decoded frame (`src/renderer.c:630`), regardless of whether the HUD is visible — the `hud_hidden` toggle (`src/main.c:480-483`) does not gate it.
- **Sample-count arithmetic.** 4K at stride 8 = 480 × 270 = 129,600 samples; × 60 fps = 7.78 M/s. Matches the design's ~130k / 7.8M.
- **`floor_cutoff_nits = 0.05` exists and is exactly the fragility the design describes** (`src/probe.c:200`, with a comment saying so).
- **The naive-accumulated-min argument is correct.** A running min over `floor_nits` would find ~0.051 on almost any file, and `dr_stops = log2(peak/floor)` (`src/probe.c:236-237`) would degenerate to a near-constant.
- **`cll_max` / `cll_avg` exist and are parsed from stream side data** (`src/decoder.h:35`, `src/decoder.c:86-91`), guarded by `has_cll`.
- **`diagnose.c` establishes the idiom claimed**: `check()` with PASS/WARN/FAIL and file-static counters (`src/diagnose.c:17-32`), `summary: %d FAIL, %d WARN` footer and `return fail_count` (`src/diagnose.c:236-237`).
- **Key bindings.** The taken set is exactly `ESC/Q F H S P O SPACE L R ←/→ M I` (`src/main.c:422-483`). `A` is free.
- **Bottom-right is the free corner.** Status panel is at (16,16) (`src/hud.c:338`); LR badges are both at `y=16` near the horizontal centre, TB badges both at `x=16` near the vertical centre, DIAG puts HDR lower-left and SDR upper-right (`src/hud.c:390-406`). Nothing occupies bottom-right in any mode.
- **`draw_text_color` silently clips at the panel edge** (`src/hud.c:103`), and the status panel is a fixed 460×220 (`src/hud.c:204`). The truncation the design flags as a pre-existing bug is real — see S11 for the exact line count.
- **No test infrastructure exists** (no `tests/`, no `enable_testing()` in `CMakeLists.txt`).
- **`AVStream::nb_frames` unreliability** is a fair characterization (absent for MPEG-TS and many MKV muxes, reliable for MP4/MOV via `stsz`), and "estimate from duration × fps" is the conventional fallback. The problems are in how the fallback interacts with coverage and VFR (C4, S8), not in the fallback itself.
- **The one-sided-inference structure of the MaxCLL check is salvageable.** Because all three measurement biases (stride, luma-vs-maxRGB, Jensen on a convex EOTF) push the estimate *down*, `measured > declared` really is valid evidence of under-declaration. That part of the design's reasoning ("under-declaring is the FAIL case, over-declaring is merely WARN") is directionally right — it's the `PASS` label on the uninformative branch that has to go (C3).
