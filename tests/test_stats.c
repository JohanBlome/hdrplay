/* Tests for the probe / session-statistics layer.
 *
 * probe.c is included as a translation unit rather than linked so the
 * tests can reach its statics — the three EOTFs and the pixel-format
 * predicates. Exhaustively verifying the transfer LUT against the
 * direct EOTF is the whole point of having a LUT at all, and that check
 * is only possible from inside.
 */
#include "probe.c"
#include "stats.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond, fmt, ...) do {                                  \
    if (cond) printf("  ok    " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); fails++; }   \
} while (0)

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static void put_component(AVFrame *f, const AVPixFmtDescriptor *d,
                          int c, int x, int y, unsigned value)
{
    const AVComponentDescriptor *cd = &d->comp[c];
    uint8_t *p = f->data[cd->plane] +
                 (size_t)y * f->linesize[cd->plane] +
                 (size_t)x * cd->step + cd->offset;
    unsigned stored = value << cd->shift;
    int bytes = (cd->depth + cd->shift + 7) / 8;
    for (int i = 0; i < bytes; i++) p[i] = (stored >> (8 * i)) & 0xff;
}

/* ------------------------------------------------------------------ */
static AVFrame *mkframe(enum AVPixelFormat fmt, int w, int h,
                        enum AVColorTransferCharacteristic trc, uint16_t yval)
{
    AVFrame *f = av_frame_alloc();
    f->format = fmt; f->width = w; f->height = h;
    f->color_trc = trc;
    f->colorspace = AVCOL_SPC_BT2020_NCL;
    f->color_primaries = AVCOL_PRI_BT2020;
    f->color_range = AVCOL_RANGE_MPEG;
    av_frame_get_buffer(f, 32);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            put_component(f, d, 0, x, y, yval);
    for (int c = 1; c <= 2; c++) {
        int cw = AV_CEIL_RSHIFT(w, d->log2_chroma_w);
        int ch = AV_CEIL_RSHIFT(h, d->log2_chroma_h);
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                put_component(f, d, c, x, y, 1u << (d->comp[c].depth - 1));
    }
    return f;
}

/* ------------------------------------------------------------------ */
static void test_lut_equivalence(void)
{
    puts("transfer LUT is exactly equivalent to the direct EOTF");
    int depths[] = { 8, 10, 12 };
    enum AVColorTransferCharacteristic trcs[] = {
        AVCOL_TRC_SMPTE2084, AVCOL_TRC_ARIB_STD_B67, AVCOL_TRC_BT709,
    };
    long compared = 0;
    double worst = 0.0;

    for (int di = 0; di < 3; di++) {
        for (int ri = 0; ri < 2; ri++) {
            for (int ti = 0; ti < 3; ti++) {
                int  depth = depths[di];
                bool full  = (ri == 1);
                double lw  = 1000.0;
                const LumaLut *L = luma_lut_get(depth, full, trcs[ti], lw);

                int max_raw = (1 << depth) - 1;
                int y_lo = 16 << (depth - 8), y_hi = 235 << (depth - 8);
                bool is_hlg = (trcs[ti] == AVCOL_TRC_ARIB_STD_B67);
                double (*eotf)(double) =
                    (trcs[ti] == AVCOL_TRC_SMPTE2084) ? pq_eotf :
                    is_hlg ? hlg_inverse_oetf : sdr_eotf;

                for (int raw = 0; raw <= max_raw; raw++) {
                    double Y = full ? (double)raw / max_raw
                                    : (double)(raw - y_lo) / (double)(y_hi - y_lo);
                    if (Y < 0) Y = 0;
                    if (Y > 1) Y = 1;
                    double want = eotf(Y);
                    if (is_hlg) want = hlg_display_nits(want, lw);
                    /* Stored as float, so compare in float terms. */
                    double got = L->nits[raw];
                    double err = fabs(got - want) /
                                 (want > 1e-9 ? want : 1.0);
                    if (err > worst) worst = err;
                    compared++;
                }
            }
        }
    }
    CHECK(compared == 32256, "compared %ld entries (expect 32256)", compared);
    CHECK(worst < 1e-6, "worst relative error %.3g (expect < 1e-6)", worst);
}

static void test_hlg_ootf(void)
{
    puts("HLG OOTF");
    CHECK(NEAR(hlg_system_gamma(1000.0), 1.2, 1e-12),
          "system gamma at 1000N = %.6f", hlg_system_gamma(1000.0));
    CHECK(NEAR(hlg_display_nits(1.0, 1000.0), 1000.0, 1e-9),
          "Y_S=1.0 -> %.2fN (expect L_W)", hlg_display_nits(1.0, 1000.0));
    CHECK(NEAR(hlg_display_nits(0.1, 1000.0), 63.096, 0.01),
          "Y_S=0.1 -> %.3fN (expect 63.1, NOT the 100 a linear scale gives)",
          hlg_display_nits(0.1, 1000.0));
    CHECK(NEAR(hlg_display_nits(0.01, 1000.0), 3.981, 0.01),
          "Y_S=0.01 -> %.3fN (expect 3.98, not 10)",
          hlg_display_nits(0.01, 1000.0));
    CHECK(hlg_display_nits(0.0, 1000.0) == 0.0, "Y_S=0 -> 0N");

    /* Full-scale HLG white through the real path. */
    AVFrame *f = mkframe(AV_PIX_FMT_YUV420P10LE, 32, 32,
                         AVCOL_TRC_ARIB_STD_B67, 940);
    FrameStats fs;
    CHECK(probe_frame_stats(f, 1, PROBE_LUMA_ONLY, &fs), "HLG frame measured");
    CHECK(NEAR(fs.peak_nits, 1000.0, 1.0),
          "HLG full-scale white -> %.1fN (was 1.0 before the OOTF fix)",
          fs.peak_nits);
    CHECK(fs.reference == LUM_HLG_OOTF, "tagged LUM_HLG_OOTF");
    av_frame_free(&f);
}

static void test_format_guard(void)
{
    puts("pixel-format guard");
    struct { enum AVPixelFormat f; bool luma, chroma; const char *n; } c[] = {
        { AV_PIX_FMT_YUV420P10LE, true,  true,  "yuv420p10le" },
        { AV_PIX_FMT_YUV420P,     true,  true,  "yuv420p"     },
        { AV_PIX_FMT_YUV422P10LE, true,  true,  "yuv422p10le" },
        { AV_PIX_FMT_YUV444P10LE, true,  true,  "yuv444p10le" },
        { AV_PIX_FMT_NV12,        true,  true,  "nv12"        },
        { AV_PIX_FMT_P010LE,      true,  true,  "p010le"      },
        { AV_PIX_FMT_YUV420P10BE, false, false, "yuv420p10be" },
        { AV_PIX_FMT_GBRP10LE,    false, false, "gbrp10le"    },
    };
    for (size_t i = 0; i < sizeof(c)/sizeof(*c); i++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(c[i].f);
        int depth = 0;
        bool l = luma_plane_supported(d, &depth);
        bool ch = l && chroma_planes_supported(d);
        CHECK(l == c[i].luma && ch == c[i].chroma,
              "%-12s luma=%d chroma=%d", c[i].n, l, ch);
    }

    /* P010's 10 bits sit at shift 6. They must be shifted down before
     * indexing the transfer LUT, including on the interleaved UV plane. */
    FrameStats fs;
    AVFrame *p = mkframe(AV_PIX_FMT_P010LE, 32, 32, AVCOL_TRC_SMPTE2084, 940);
    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(p->format);
    put_component(p, pd, 1, 2, 3, 100);
    put_component(p, pd, 2, 2, 3, 900);
    CHECK(read_component(p, pd, 1, 4, 6) == 100 &&
          read_component(p, pd, 2, 4, 6) == 900,
          "P010 interleaved chroma offsets and shifts decoded");
    CHECK(probe_frame_stats(p, 1, PROBE_FULL_RGB, &fs), "P010 full-RGB works");
    CHECK(NEAR(fs.peak_nits, 10000.0, 1.0),
          "P010 code 940 -> %.1fN (expect 10000N)", fs.peak_nits);
    av_frame_free(&p);

    /* NV12 uses the same semi-planar layout at 8 bit. */
    AVFrame *n = mkframe(AV_PIX_FMT_NV12, 32, 32, AVCOL_TRC_SMPTE2084, 235);
    const AVPixFmtDescriptor *nd = av_pix_fmt_desc_get(n->format);
    put_component(n, nd, 1, 2, 3, 20);
    put_component(n, nd, 2, 2, 3, 220);
    CHECK(read_component(n, nd, 1, 4, 6) == 20 &&
          read_component(n, nd, 2, 4, 6) == 220,
          "NV12 interleaved chroma offsets decoded");
    CHECK(probe_frame_stats(n, 1, PROBE_LUMA_ONLY, &fs), "NV12 luma-only works");
    CHECK(probe_frame_stats(n, 1, PROBE_FULL_RGB, &fs), "NV12 full-RGB works");
    av_frame_free(&n);
}

static void test_histogram(void)
{
    puts("histogram and percentiles");
    CHECK(probe_nits_to_bin(-1.0) < 0, "negative nits -> underflow");
    CHECK(probe_nits_to_bin(0.0) < 0,  "zero nits -> underflow");
    CHECK(probe_nits_to_bin(1e-9) < 0, "below range -> underflow, not bin 0");
    int b1 = probe_nits_to_bin(1.0);
    CHECK(NEAR(probe_bin_to_nits(b1), 1.0, 0.05),
          "1 nit round-trips to %.4f", probe_bin_to_nits(b1));
    CHECK(probe_nits_to_bin(1e30) == PROBE_HIST_BINS - 1, "saturates at top bin");

    /* Known distribution: 1000 samples at 100 nits, 1000 at 1000 nits.
     * Median sits in the lower group, p99 in the upper. */
    ProbeHist h; memset(&h, 0, sizeof(h));
    for (int i = 0; i < 1000; i++) hist_add(&h, probe_nits_to_bin(100.0), false);
    for (int i = 0; i < 1000; i++) hist_add(&h, probe_nits_to_bin(1000.0), false);
    CHECK(h.valid == 2000, "valid population = %llu", (unsigned long long)h.valid);
    CHECK(NEAR(probe_hist_percentile(&h, 25.0), 100.0, 5.0),
          "p25 = %.1fN", probe_hist_percentile(&h, 25.0));
    CHECK(NEAR(probe_hist_percentile(&h, 99.0), 1000.0, 40.0),
          "p99 = %.1fN", probe_hist_percentile(&h, 99.0));

    /* Mean/variance in log2, against the closed form for a two-point
     * distribution: mean = (log2(100)+log2(1000))/2, and the variance is
     * half the squared separation. */
    double want_mean = (log2(100.0) + log2(1000.0)) / 2.0;
    double half = (log2(1000.0) - log2(100.0)) / 2.0;
    CHECK(NEAR(probe_hist_mean_log2(&h), want_mean, 0.05),
          "mean log2 = %.4f (expect %.4f)", probe_hist_mean_log2(&h), want_mean);
    CHECK(NEAR(probe_hist_var_log2(&h), half * half, 0.05),
          "var log2 = %.4f (expect %.4f)", probe_hist_var_log2(&h), half * half);

    /* Empty population must not divide by zero. */
    ProbeHist e; memset(&e, 0, sizeof(e));
    CHECK(isnan(probe_hist_percentile(&e, 50.0)), "empty -> NAN percentile");
    CHECK(isnan(probe_hist_mean_log2(&e)), "empty -> NAN mean");
}

static void test_black_and_underflow(void)
{
    puts("black / underflow separation");
    /* All-black 10-bit limited-range frame: y_raw == y_lo == 64. Every
     * sample is code-domain black, so the valid population is empty and
     * nothing may divide by zero. */
    AVFrame *f = mkframe(AV_PIX_FMT_YUV420P10LE, 32, 32,
                         AVCOL_TRC_SMPTE2084, 64);
    FrameStats fs;
    CHECK(probe_frame_stats(f, 1, PROBE_LUMA_ONLY, &fs), "all-black measured");
    CHECK(fs.luma.valid == 0, "no valid samples (%llu)",
          (unsigned long long)fs.luma.valid);
    CHECK(fs.luma.black == (uint64_t)fs.samples, "all samples counted black");
    CHECK(fs.dr_stops == 0.0, "DR is 0, not NAN or inf (%f)", fs.dr_stops);
    av_frame_free(&f);

    /* One code above black. Whether that is resolvable depends on the
     * transfer, and both outcomes must be handled correctly.
     *
     * PQ: y_raw=65 at 10-bit limited is 5.3e-5 nits, comfortably above
     * the 2^-16 (1.5e-5) bottom of the histogram — so it is a genuine
     * low bin, NOT the clamp bucket. The range was chosen to reach
     * this far down precisely so shadow detail stays resolvable. */
    AVFrame *g = mkframe(AV_PIX_FMT_YUV420P10LE, 32, 32,
                         AVCOL_TRC_SMPTE2084, 65);
    CHECK(probe_frame_stats(g, 1, PROBE_LUMA_ONLY, &fs), "PQ near-black measured");
    CHECK(fs.luma.black == 0, "PQ near-black not counted as black");
    CHECK(fs.luma.underflow == 0, "PQ near-black resolved, not underflow");
    CHECK(fs.luma.valid == (uint64_t)fs.samples, "PQ near-black is in a real bin");
    av_frame_free(&g);

    /* SDR at the BT.1886 default: the same code sits just above the
     * 0.1-nit black level, so it is an ordinary low bin. That is the
     * point of having a floor — the darkest codes stay resolvable
     * instead of collapsing toward zero. */
    AVFrame *sdr = mkframe(AV_PIX_FMT_YUV420P10LE, 32, 32,
                           AVCOL_TRC_BT709, 65);
    CHECK(probe_frame_stats(sdr, 1, PROBE_LUMA_ONLY, &fs), "SDR near-black measured");
    CHECK(fs.luma.black == 0, "SDR near-black not counted as black");
    CHECK(fs.luma.underflow == 0, "BT.1886 floor keeps SDR near-black resolvable");
    CHECK(fs.luma.valid == (uint64_t)fs.samples,
          "SDR near-black is in a real bin");
    CHECK(NEAR(fs.peak_nits, 0.105, 0.01),
          "SDR near-black reads %.4fN, just above the 0.1N floor", fs.peak_nits);

    /* With the floor removed it is 8.7e-6 nits, below the bottom of the
     * histogram. This is where the underflow bucket earns its keep:
     * without it these samples would pile into clamp-bin 0 at a nominal
     * 1.5e-5 nits and drag p1 down to a near-constant, making the
     * dynamic range figure meaningless on every SDR clip. */
    probe_set_sdr_black_nits(0.0);
    CHECK(probe_frame_stats(sdr, 1, PROBE_LUMA_ONLY, &fs),
          "SDR near-black measured with no floor");
    CHECK(fs.luma.underflow == (uint64_t)fs.samples,
          "floorless SDR near-black counted as underflow (%llu), kept out "
          "of percentiles", (unsigned long long)fs.luma.underflow);
    CHECK(fs.luma.valid == 0,
          "floorless SDR near-black excluded from the percentile population");
    probe_set_sdr_black_nits(0.1);
    av_frame_free(&sdr);
}

static void test_sdr_black_level(void)
{
    puts("BT.1886 black level anchors the SDR transfer");
    /* The two endpoints are the definition of the curve: L(0) = L_B and
     * L(1) = L_W. Everything the dynamic-range figure rests on follows
     * from the first of those being non-zero. */
    probe_set_sdr_black_nits(0.1);
    CHECK(NEAR(sdr_eotf(0.0), 0.1, 1e-9), "L(0) = %.6fN (expect 0.1)", sdr_eotf(0.0));
    CHECK(NEAR(sdr_eotf(1.0), 100.0, 1e-9), "L(1) = %.4fN (expect 100)", sdr_eotf(1.0));
    CHECK(sdr_eotf(0.5) > 0.0 && sdr_eotf(0.5) < 100.0, "monotone in between");

    probe_set_sdr_black_nits(0.0);
    CHECK(NEAR(sdr_eotf(0.0), 0.0, 1e-12), "L_B=0 -> L(0) = 0");
    CHECK(NEAR(sdr_eotf(0.5), 100.0 * pow(0.5, 2.4), 1e-9),
          "L_B=0 collapses to the bare 100*V^2.4");
    probe_set_sdr_black_nits(0.1);
}

static void test_dr_ceiling(void)
{
    puts("dynamic-range ceiling bounds what a coding can express");
    probe_set_sdr_black_nits(0.1);

    /* 8-bit limited SDR. The darkest non-black code is 17, one step up
     * from black in a 219-code window; against 100-nit white that is
     * just under the 10 stops the 0.1-nit floor allows outright. */
    double c8 = probe_dr_ceiling_stops(AVCOL_TRC_BT709, 8, false, 0.0);
    CHECK(NEAR(c8, 9.75, 0.1), "8-bit limited SDR ceiling %.2f stops (expect ~9.7)", c8);
    CHECK(c8 < log2(100.0 / 0.1) + 1e-9,
          "never exceeds log2(L_W/L_B) = 10.0 stops");

    /* More codes resolve a darker step, so the ceiling rises with depth
     * and rises again over the wider full-range window. */
    double c10 = probe_dr_ceiling_stops(AVCOL_TRC_BT709, 10, false, 0.0);
    CHECK(c10 > c8, "10-bit ceiling %.2f > 8-bit %.2f", c10, c8);
    CHECK(probe_dr_ceiling_stops(AVCOL_TRC_BT709, 8, true, 0.0) > c8,
          "full range resolves further than limited at the same depth");

    /* Removing the floor is exactly what turns the ceiling into a
     * statement about the code lattice: 2.4 * log2(219) = 18.7. */
    probe_set_sdr_black_nits(0.0);
    double bare = probe_dr_ceiling_stops(AVCOL_TRC_BT709, 8, false, 0.0);
    CHECK(NEAR(bare, 2.4 * log2(219.0), 0.01),
          "floorless 8-bit ceiling %.2f = 2.4*log2(219)", bare);
    probe_set_sdr_black_nits(0.1);

    /* PQ carries its own absolute floor, so it is far deeper. */
    CHECK(probe_dr_ceiling_stops(AVCOL_TRC_SMPTE2084, 10, false, 0.0) > 20.0,
          "PQ 10-bit ceiling is deep (%.1f stops)",
          probe_dr_ceiling_stops(AVCOL_TRC_SMPTE2084, 10, false, 0.0));

    /* HLG's ceiling must ride the OOTF. The inverse OETF alone is fixed, so the
     * whole L_W dependence enters through the system gamma — the ceiling scales
     * by exactly g(L_W)/g(1000) and by nothing else. Skipping the OOTF (the bug
     * this pins) would leave the ratio at 1.0. */
    double h1k = probe_dr_ceiling_stops(AVCOL_TRC_ARIB_STD_B67, 10, false, 1000.0);
    double h4k = probe_dr_ceiling_stops(AVCOL_TRC_ARIB_STD_B67, 10, false, 4000.0);
    double want = hlg_system_gamma(4000.0) / hlg_system_gamma(1000.0);
    CHECK(NEAR(h1k, 25.3614, 0.01), "HLG 10-bit ceiling at L_W=1000 is %.4f", h1k);
    CHECK(NEAR(h4k / h1k, want, 1e-6),
          "HLG ceiling scales by the system gamma: %.4f/%.4f = %.6f (expect %.6f)",
          h4k, h1k, h4k / h1k, want);
    CHECK(NEAR(hlg_system_gamma(1000.0), 1.2, 1e-12),
          "system gamma is 1.2 at the reference peak");
}

static void test_dr_within_ceiling(void)
{
    puts("measured dynamic range stays inside the ceiling");
    probe_set_sdr_black_nits(0.1);

    /* A full 8-bit limited-range ramp is the worst case: it puts real
     * population on the darkest and brightest codes there are, so the
     * measurement should press right up against the ceiling without
     * ever passing it. That was the failure — 16.2 measured against a
     * format that can only carry 9.7. */
    AVFrame *f = mkframe(AV_PIX_FMT_YUV420P, 256, 256, AVCOL_TRC_BT709, 0);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
            f->data[0][y * f->linesize[0] + x] = (uint8_t)(16 + (x * 219) / 255);

    FrameStats fs;
    CHECK(probe_frame_stats(f, 1, PROBE_LUMA_ONLY, &fs), "ramp measured");

    SessionStats s;
    session_stats_init(&s, 1.0 / 1000.0, 1.0);
    session_stats_add(&s, 0, 0, &fs);
    SessionDerived d;
    session_stats_derive(&s, &d);

    CHECK(d.bit_depth == 8, "depth carried through as %d", d.bit_depth);
    CHECK(!d.full_range, "limited range carried through");
    CHECK(d.dr_ceiling_stops > 0.0 && isfinite(d.dr_ceiling_stops),
          "ceiling derived (%.2f stops)", d.dr_ceiling_stops);
    CHECK(d.dr_stops <= d.dr_ceiling_stops + 0.25,
          "measured %.2f within ceiling %.2f", d.dr_stops, d.dr_ceiling_stops);
    CHECK(d.dr_stops > d.dr_ceiling_stops - 1.5,
          "a full ramp gets close to it: %.2f vs %.2f",
          d.dr_stops, d.dr_ceiling_stops);
    av_frame_free(&f);
}

static void test_range_guess(void)
{
    puts("colour range recovered from the pixels when untagged");

    /* Limited-range content: a ramp confined to 16..235 plus a few
     * ringing samples just outside. Nothing deep enough to count. */
    AVFrame *lim = mkframe(AV_PIX_FMT_YUV420P, 256, 256, AVCOL_TRC_BT709, 0);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
            lim->data[0][y * lim->linesize[0] + x] = (uint8_t)(16 + (x * 219) / 255);
    /* One row of overshoot on each side — real encodes do this. */
    for (int x = 0; x < 256; x++) {
        lim->data[0][0 * lim->linesize[0] + x] = 14;
        lim->data[0][1 * lim->linesize[0] + x] = 237;
    }
    double frac = -1.0;
    CHECK(probe_guess_color_range(lim, &frac) == AVCOL_RANGE_MPEG,
          "ramp inside 16..235 reads as limited (%.4f%% outside)", frac * 100.0);
    CHECK(frac < PROBE_RANGE_FULL_FRAC, "ringing alone stays under the bar");
    av_frame_free(&lim);

    /* Full-range content: the same ramp over the whole 0..255 window.
     * The deep-shadow population is what gives it away. */
    AVFrame *full = mkframe(AV_PIX_FMT_YUV420P, 256, 256, AVCOL_TRC_BT709, 0);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
            full->data[0][y * full->linesize[0] + x] = (uint8_t)x;
    CHECK(probe_guess_color_range(full, &frac) == AVCOL_RANGE_JPEG,
          "ramp spanning 0..255 reads as full (%.2f%% outside)", frac * 100.0);
    av_frame_free(&full);

    /* Mid-grey only: genuinely undecidable from the pixels, and the
     * standard reading of an absent flag is limited. The test exists to
     * pin that the guess is one-sided rather than a coin toss. */
    AVFrame *flat = mkframe(AV_PIX_FMT_YUV420P, 256, 256, AVCOL_TRC_BT709, 128);
    CHECK(probe_guess_color_range(flat, &frac) == AVCOL_RANGE_MPEG,
          "no evidence either way falls back to limited");
    av_frame_free(&flat);

    /* Unreadable pixel format must abstain, not guess. */
    AVFrame *nv = mkframe(AV_PIX_FMT_RGB24, 32, 32, AVCOL_TRC_BT709, 0);
    CHECK(probe_guess_color_range(nv, &frac) == AVCOL_RANGE_UNSPECIFIED,
          "unreadable format abstains");
    av_frame_free(&nv);
}

static void test_session_dedupe(void)
{
    puts("session accumulation and PTS dedupe");
    AVFrame *f = mkframe(AV_PIX_FMT_YUV420P10LE, 32, 32,
                         AVCOL_TRC_SMPTE2084, 800);
    FrameStats fs;
    probe_frame_stats(f, 1, PROBE_LUMA_ONLY, &fs);

    SessionStats s;
    session_stats_init(&s, 1.0 / 1000.0, 10.0);

    CHECK(session_stats_add(&s, 1000, 0, &fs), "first frame accepted");
    CHECK(!session_stats_add(&s, 1000, 0, &fs), "same PTS rejected");
    CHECK(!session_stats_add(&s, 500, 0, &fs), "earlier PTS rejected (backward seek)");
    CHECK(session_stats_add(&s, 2000, 0, &fs), "later PTS accepted");
    CHECK(s.frames == 2, "frames = %llu (expect 2)", (unsigned long long)s.frames);

    /* Re-running the same range, as --loop does, must not reweight. */
    uint64_t valid_before = s.valid;
    for (int i = 0; i < 5; i++) session_stats_add(&s, 1000 + i * 100, 0, &fs);
    CHECK(s.valid == valid_before, "loop replay added nothing");

    SessionDerived d;
    session_stats_derive(&s, &d);
    CHECK(NEAR(d.coverage, 0.1, 1e-6), "coverage = %.4f (1000 ticks of 10s)",
          d.coverage);

    /* Unknown duration must not fabricate a coverage figure. */
    SessionStats u;
    session_stats_init(&u, 1.0 / 1000.0, 0.0);
    session_stats_add(&u, 0, 0, &fs);
    session_stats_derive(&u, &d);
    CHECK(d.coverage < 0.0, "unknown duration -> coverage %.1f", d.coverage);

    av_frame_free(&f);
}

static void test_variance_decomposition(void)
{
    puts("variance decomposition: total^2 == spatial^2 + temporal^2");
    /* Two frames with different mean brightness, each internally
     * varied, so both terms are non-zero and the identity is a real
     * constraint rather than 0 == 0. */
    SessionStats s;
    session_stats_init(&s, 1.0 / 1000.0, 10.0);

    for (int k = 0; k < 2; k++) {
        AVFrame *f = mkframe(AV_PIX_FMT_YUV420P10LE, 64, 64,
                             AVCOL_TRC_SMPTE2084, 0);
        /* Half the rows dark, half bright; the bright level differs per
         * frame so the frame means differ too. */
        uint16_t lo = 500, hi = (k == 0) ? 700 : 900;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                ((uint16_t *)f->data[0])[y * (f->linesize[0] / 2) + x] =
                    (y < 32) ? lo : hi;
        FrameStats fs;
        probe_frame_stats(f, 1, PROBE_LUMA_ONLY, &fs);
        session_stats_add(&s, 1000 * (k + 1), 0, &fs);
        av_frame_free(&f);
    }

    SessionDerived d;
    session_stats_derive(&s, &d);
    double lhs = d.total_stops * d.total_stops;
    double rhs = d.spatial_stops * d.spatial_stops +
                 d.temporal_stops * d.temporal_stops;
    CHECK(d.spatial_stops > 0.01, "spatial term non-trivial (%.4f)", d.spatial_stops);
    CHECK(d.temporal_stops > 0.01, "temporal term non-trivial (%.4f)", d.temporal_stops);
    CHECK(NEAR(lhs, rhs, 1e-9), "total^2 %.9f == spatial^2+temporal^2 %.9f",
          lhs, rhs);
}

static void test_maxrgb(void)
{
    puts("maxRGB path (MaxCLL is defined over max(R,G,B), not luma)");
    /* Saturated blue: Y = 0.0593*B for BT.2020, so luma reads far below
     * the blue channel. This is the bias that makes luma-only MaxCLL a
     * lower bound. */
    AVFrame *f = mkframe(AV_PIX_FMT_YUV444P10LE, 32, 32,
                         AVCOL_TRC_SMPTE2084, 300);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            ((uint16_t *)f->data[1])[y * (f->linesize[1] / 2) + x] = 940;

    FrameStats fs;
    CHECK(probe_frame_stats(f, 1, PROBE_FULL_RGB, &fs), "full-RGB measured");
    CHECK(fs.has_maxrgb, "maxRGB available");
    CHECK(fs.maxrgb_peak > fs.peak_nits,
          "maxRGB peak %.1fN exceeds luma peak %.1fN on saturated blue",
          fs.maxrgb_peak, fs.peak_nits);

    FrameStats lf;
    probe_frame_stats(f, 1, PROBE_LUMA_ONLY, &lf);
    CHECK(!lf.has_maxrgb, "luma-only reports no maxRGB rather than guessing");
    av_frame_free(&f);
}

static void test_rgb_waveform(void)
{
    puts("RGB waveform");
    AVFrame *f = mkframe(AV_PIX_FMT_P010LE, 16, 16,
                         AVCOL_TRC_ARIB_STD_B67, 64);
    enum { W = 8, H = 121 };
    uint32_t bins[3 * W * H];
    uint32_t peak[3];
    int samples = 0;

    CHECK(probe_rgb_waveform(f, NULL, W, H, 2, bins, peak, &samples),
          "P010 source produces an RGB waveform");
    CHECK(samples == W * 8, "sample count %d (expect %d)", samples, W * 8);

    /* Limited-range neutral code 64 in 10-bit is nominal 0%, which maps
     * to row 10 in a 121-row -10%..110% graph. All channels coincide. */
    int zero_row = 110;
    bool neutral = true;
    for (int c = 0; c < 3; c++)
        for (int x = 0; x < W; x++)
            neutral &= bins[(c * H + zero_row) * W + x] == 8;
    CHECK(neutral, "nominal black aligns R, G and B at 0%%");
    CHECK(peak[0] == 8 && peak[1] == 8 && peak[2] == 8,
          "per-channel density peaks preserved");

    ProbeRegion roi = { 4, 4, 12, 12 };
    CHECK(probe_rgb_waveform(f, &roi, W, H, 2,
                             bins, peak, &samples),
          "waveform accepts a source region");
    CHECK(samples == W * 4,
          "waveform samples only the selected rows (%d)", samples);

    av_frame_free(&f);
}

static void test_rgb_histogram(void)
{
    puts("RGB histogram");
    AVFrame *f = mkframe(AV_PIX_FMT_P010LE, 16, 16,
                         AVCOL_TRC_ARIB_STD_B67, 64);
    enum { W = 121 };
    uint32_t bins[3 * W], peak[3];
    ProbeRgbHistogramStats stats;

    CHECK(probe_rgb_histogram(f, NULL, W, 2, bins, peak, &stats),
          "P010 source produces an RGB histogram");
    CHECK(stats.samples == 64, "sample count %llu (expect 64)",
          (unsigned long long)stats.samples);
    /* Neutral nominal black is 0%, bin 10 in a 121-bin -10%..110%
     * histogram. All three channels coincide there. */
    bool neutral = true;
    for (int c = 0; c < 3; c++) {
        neutral &= bins[c * W + 10] == 64;
        neutral &= peak[c] == 64;
        neutral &= stats.outside_nominal[c] == 0;
    }
    CHECK(neutral, "nominal black aligns all channels at 0%%");
    ProbeRegion roi = { 4, 4, 12, 12 };
    CHECK(probe_rgb_histogram(f, &roi, W, 2, bins, peak, &stats),
          "histogram accepts a source region");
    CHECK(stats.samples == 16,
          "histogram samples only the selected rectangle (%llu)",
          (unsigned long long)stats.samples);
    av_frame_free(&f);
}

static void test_xy_gamut(void)
{
    puts("CIE xy gamut scope");
    CHECK(xy_in_triangle(.300, .600, .640, .330, .300, .600,
                         .150, .060),
          "Rec.709 green vertex is inside Rec.709");
    CHECK(!xy_in_triangle(.680, .320, .640, .330, .300, .600,
                          .150, .060),
          "Display-P3 red extends outside Rec.709");
    CHECK(xy_in_triangle(.680, .320, .680, .320, .265, .690,
                         .150, .060),
          "Display-P3 red is inside Display-P3");

    AVFrame *f = mkframe(AV_PIX_FMT_P010LE, 16, 16,
                         AVCOL_TRC_ARIB_STD_B67, 502);
    enum { W = 64, H = 64 };
    uint32_t bins[W * H];
    uint32_t peak = 0;
    ProbeGamutStats stats;
    CHECK(probe_xy_gamut(f, NULL, W, H, 2, bins, &peak, &stats),
          "P010 source produces a CIE gamut density plot");
    uint64_t total = 0;
    for (int i = 0; i < W * H; i++) total += bins[i];
    CHECK(stats.samples == 64 && total == 64,
          "all 64 non-black samples land in the plot");
    CHECK(stats.outside_709 == 0 && stats.outside_p3 == 0,
          "neutral samples remain inside Rec.709 and Display-P3");
    CHECK(peak == 64, "coincident neutral samples retain density");
    ProbeRegion roi = { 4, 4, 12, 12 };
    CHECK(probe_xy_gamut(f, &roi, W, H, 2, bins, &peak, &stats) &&
          stats.samples == 16,
          "gamut scope samples only the selected rectangle");
    av_frame_free(&f);
}

static void test_vectorscope(void)
{
    puts("Cb/Cr vectorscope");
    double targets709[6][2], targets2020[6][2];
    CHECK(probe_vectorscope_targets(AVCOL_SPC_BT709, .75, targets709),
          "Rec.709 75%% targets available");
    CHECK(probe_vectorscope_targets(AVCOL_SPC_BT2020_NCL, .75,
                                    targets2020),
          "BT.2020 NCL 75%% targets available");
    CHECK(NEAR(targets709[0][1], .375, 1e-12) &&
          NEAR(targets2020[0][1], .375, 1e-12),
          "75%% red target has Cr=0.375 in both matrices");
    CHECK(!NEAR(targets709[0][0], targets2020[0][0], 1e-3),
          "Rec.709 and BT.2020 red targets use different Cb positions");
    CHECK(!probe_vectorscope_targets(AVCOL_SPC_BT2020_CL, .75,
                                     targets2020),
          "BT.2020 constant-luminance does not claim NCL targets");

    AVFrame *f = mkframe(AV_PIX_FMT_P010LE, 16, 16,
                         AVCOL_TRC_ARIB_STD_B67, 502);
    enum { W = 64, H = 64 };
    uint32_t bins[PROBE_VECTOR_BANDS * W * H];
    uint32_t peak[PROBE_VECTOR_BANDS] = {0};
    ProbeVectorStats stats;
    CHECK(probe_cbcr_vectorscope(f, NULL, W, H, 2, 1.0,
                                 bins, peak, &stats),
          "P010 source produces a Cb/Cr density plot");
    uint64_t total = 0;
    for (int i = 0; i < PROBE_VECTOR_BANDS * W * H; i++) total += bins[i];
    CHECK(stats.samples == 64 && total == 64,
          "all neutral samples land in the vectorscope");
    CHECK(stats.outside_nominal == 0,
          "neutral samples remain inside nominal chroma range");
    CHECK(peak[PROBE_VECTOR_SHADOW] == 0 &&
          peak[PROBE_VECTOR_MID] == 64 &&
          peak[PROBE_VECTOR_HIGHLIGHT] == 0,
          "50%% Y' neutral chroma occupies the midtone band");
    CHECK(stats.luma_band[PROBE_VECTOR_MID] == 64,
          "vectorscope reports its Y' band population");
    ProbeRegion roi = { 4, 4, 12, 12 };
    CHECK(probe_cbcr_vectorscope(f, &roi, W, H, 2, 1.0,
                                 bins, peak, &stats) &&
          stats.samples == 16,
          "vectorscope samples only the selected rectangle");
    av_frame_free(&f);
}

int main(void)
{
    test_lut_equivalence();
    test_hlg_ootf();
    test_format_guard();
    test_histogram();
    test_black_and_underflow();
    test_sdr_black_level();
    test_dr_ceiling();
    test_dr_within_ceiling();
    test_range_guess();
    test_session_dedupe();
    test_variance_decomposition();
    test_maxrgb();
    test_rgb_waveform();
    test_rgb_histogram();
    test_xy_gamut();
    test_vectorscope();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
