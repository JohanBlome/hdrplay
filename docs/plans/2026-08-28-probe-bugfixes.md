# Probe / HUD bug fixes

**Date:** 2026-08-28
**Origin:** defects surfaced by the adversarial review of the accumulated-stats
design (`accumulated-stats-design-review-1.md`). These are live bugs in code
that already shipped, fixed independently of that feature.

Verified with a throwaway harness over synthetic `AVFrame`s (see
[Verification](#verification)). Permanent test scaffolding is step 1 of
the accumulated-stats plan, not part of this change.

---

## 1. HLG sources reported ~1 nit instead of display luminance

**Files:** `src/probe.c`, `src/probe.h`

`hlg_inverse_oetf()` returns **scene**-referred linear in [0,1] — its own
comment said so — but both `probe_sample()` and `probe_frame_stats()`
stored that value straight into fields documented as cd/m².

BT.2100 requires the **OOTF** to get from scene light to display light,
and it is a power law, not a scale:

```
gamma     = 1.2 + 0.42 * log10(L_W / 1000)
Y_display = L_W * Y_scene ^ gamma
```

Consequences before the fix, on every HLG clip:

- `SRC PEAK` read ~1 nit for full-scale white.
- `DR ... STOPS` was computed over a range compressed into [0,1].
- `ABOVE 500N` was permanently 0.0%.
- The `M` probe reported ~1 nit for any pixel.

**Fix.** Added `hlg_system_gamma()` and `hlg_display_nits()`, applied
after the inverse OETF in both readers.

`probe_frame_stats()` uses the luma form directly. `probe_sample()` needs
the per-channel form, which is *not* separable — the system gamma is
driven by scene luma and then applied to each channel:

```
Y_S    = kr*R_S + kg*G_S + kb*B_S
F_D(C) = L_W * Y_S^(gamma-1) * C_S
```

`L_W` is an assumption, so it is resolved explicitly and exposed:
`probe_hlg_peak_nits()` takes the frame's mastering-display max
luminance when it declares one, else the BT.2100 reference of 1000 nits.
It is public so callers can report which premise a number rests on.

PQ and SDR paths are untouched.

## 2. P010 read every sample as 10000 nits

**File:** `src/probe.c`

`probe_frame_stats()` gated only on `comp[0].depth ∈ {8,10,12}`, then
indexed `data[0]` as a tightly packed array of that depth. It never
checked `shift`, `step`, `offset`, planarity or endianness.

P010 — named in `README.md:13` as an expected input, and a routine
hardware-decoder output — reports `depth == 10` but stores those bits in
the **high** end of a 16-bit word (`shift == 6`). Dividing by
`(1<<10)-1` overshoots by ~64×, every sample clamps to 1.0, and the PQ
EOTF turns that into 10000 nits. The entire readout was wrong, silently,
for a whole class of inputs.

**Fix.** Two predicates rather than one, because the requirements
genuinely differ:

- `luma_plane_supported()` — planar, little-endian, non-RGB,
  `shift == 0`, `offset == 0`, one sample per word. Gates
  `probe_frame_stats()`.
- `chroma_planes_supported()` — additionally requires Cb and Cr in their
  own planes at matching depth. Gates `probe_sample()`.

The split matters: **NV12's luma plane is a perfectly ordinary 8-bit
planar array**, so it still yields valid frame statistics; only its
interleaved UV plane is unreadable here. A single combined predicate
would have discarded luma stats for a common format for no reason. My
first attempt did exactly that, and the verification harness caught it.

P010 stays rejected. Reading it correctly is only a `>> 6`, but that is
a feature, not a bug fix, so it is left as a follow-up.

## 3. `probe_sample()` read the wrong chroma row for 4:2:2 and 4:4:4

**File:** `src/probe.c`

Chroma coordinates were hardcoded to a 4:2:0 halving:

```c
int cx = src_x / 2, cy = src_y / 2;   /* 4:2:0 chroma subsample */
```

4:2:2 has **full vertical** chroma resolution and 4:4:4 has no
subsampling at all, so for both the code sampled roughly half way up the
chroma plane instead of the requested row. `probe.h` documented 4:2:0 as
the only supported layout, but nothing enforced it, so ProRes-style
4:2:2 and 4:4:4 masters — likely inputs for this tool — returned
confidently wrong colours.

**Fix.** Derive the shift from the descriptor:

```c
int cx = src_x >> desc->log2_chroma_w;
int cy = src_y >> desc->log2_chroma_h;
```

Format acceptance is now enforced by `chroma_planes_supported()` rather
than left to a comment, and `probe.h` is updated to describe what is
actually supported.

## 4. Status panel silently dropped its bottom four lines

**File:** `src/hud.c`

`draw_text_color()` bounds-checks per pixel and discards anything past
the panel edge, so overflow is invisible rather than a crash. The panel
was a fixed `H = 220`; at the 24 px line pitch, line 9 is the last that
fits.

In SPLIT mode with the probe active the panel draws 12 lines (13 with
the excessive-headroom warning). So `SDR BOOST` and **both `PROBE`
lines** were being dropped — pressing `M` in split mode looked like it
did nothing.

**Fix.** `H = 220` → `340`, with a comment deriving the worst-case line
count so the next line added doesn't silently reintroduce it.

## 5. Stale comment describing a render architecture that no longer exists

**File:** `src/hud.c`

The `hud_prepare()` header comment described SPLIT mode as two
`pl_render_image` passes over half-crops, and instructed callers to
route overlays to the correct half.

`renderer.c` does **one** render into the swapchain at full crop; the
SDR side goes into the offscreen `diag_tex` and is composited as just
another overlay, with all HUD overlays on that single pass
(`src/renderer.c:729-749`).

This is included as a fix rather than a tidy-up because the comment
actively caused a defect: v1 of the accumulated-stats design specified
attaching a panel to "both passes", which would have double-blended the
panel background (alpha 170 → ~0.89) and overflowed `ov_arr[4]`.

**Fix.** Comment rewritten to describe the single-pass reality and the
ordering constraint that actually matters (the SDR overlay is opaque in
its visible region and must be composited first).

## 6. `--diagnose` emitted ANSI escapes into pipes and files

**File:** `src/diagnose.c`

`check()` wrote colour codes unconditionally, so redirecting the report
to a file or grepping it embedded `\x1b[32m` in every line.

**Fix.** `isatty(fileno(stderr))` selects plain tags when stderr is not
a terminal.

---

## Not fixed

- **Missing `>`, `<`, `%`, `,` glyphs in `FONT[]`** (`src/hud.c:30-76`).
  `find_glyph()` returns NULL for these and `draw_text_color` renders a
  solid block. Latent only — no current HUD string uses them, and the
  accumulated-stats design was adjusted to use `MIN` and `PCT` instead.
- **P010 luma support.** Rejected rather than shifted; see §2.
- **Jensen bias from decoding Y' directly** (`src/probe.c:154-162`).
  Inherent to the luma shortcut, acknowledged in the existing comment,
  and only removable with full per-channel linearization. Documented in
  the accumulated-stats design as a one-sided lower bound.

## Outside this repo

`../hdr-analysis/vca.py` has the same class of HLG bug at
`analyze_frame_luminance` (`vca.py:242`):

```python
nits = hlg_oetf_inverse(normalized) * 1000.0
```

That is the `gamma = 1` case — it scales scene light to a 1000-nit peak
but omits the system gamma, overstating everything below peak and
increasingly so toward black (Y_S=0.1 → 100 nits reported vs 63 correct;
Y_S=0.01 → 10 vs 4).

Two mitigations: it is still better than hdrplay's pre-fix behaviour
(which omitted the scale entirely), and `classify_content()`'s HLG
branch (`vca.py:288`) deliberately uses code values rather than nits, so
the verdict is unaffected — only reported and plotted numbers.

`_apply_eotf()` at `vca.py:434` returns scene linear for the gamut path,
which is **correct as-is**: the OOTF applies a common per-pixel scale to
R, G and B, so chromaticity is unchanged.

Not applied — that path is outside this session's writable directories.
Patch supplied separately.

---

## Verification

Harness at `/tmp/verify_probe.c`, compiled against the real `probe.c`
translation unit so it can reach the statics, run over synthetic
`AVFrame`s. All 22 checks pass:

- HLG full-scale white → 1000.0 nits (was 1.0).
- OOTF power law: Y_S=0.1 → 63.1 nits (a linear scale gives 100);
  Y_S=0.01 → 3.98 (vs 10); system gamma at 1000 nits = 1.2.
- PQ full-scale white → 10000 nits, unchanged.
- Format matrix: yuv420p/422p/444p at 8/10/12-bit accepted for both
  paths; NV12 luma-yes chroma-no; P010, yuv420p10be, gbrp10le rejected.
- A P010 frame is refused rather than read as 10000 nits.
- NV12 still yields frame stats but is declined by `probe_sample`.
- `probe_sample` reads the requested chroma row on 4:2:2, and accepts
  4:4:4.

`cmake --build build` is clean; the three remaining warnings
(`renderer.h:67` anonymous enum, two unused statics in `renderer.c`)
predate this change. `./build/hdrplay --diagnose | cat -v` shows no
escape sequences.
