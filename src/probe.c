#include "probe.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>

/* ------------------------------------------------------------------ */
/* PQ EOTF (BT.2100): normalized signal value → linear nits           */
/* ------------------------------------------------------------------ */
static double pq_eotf(double V)
{
    const double m1 = 2610.0  / 16384.0;
    const double m2 = 2523.0  / 4096.0 * 128.0;
    const double c1 = 3424.0  / 4096.0;
    const double c2 = 2413.0  / 4096.0 * 32.0;
    const double c3 = 2392.0  / 4096.0 * 32.0;
    if (V <= 0) return 0;
    double Vp = pow(V, 1.0 / m2);
    double num = fmax(Vp - c1, 0.0);
    double den = c2 - c3 * Vp;
    if (den <= 0) return 0;
    return 10000.0 * pow(num / den, 1.0 / m1);
}

/* HLG OETF inverse (BT.2100). Returns *scene*-referred linear in [0,1],
 * NOT display nits. Everything downstream reports cd/m², so this must
 * always be followed by the OOTF below — see hlg_display_nits(). */
static double hlg_inverse_oetf(double V)
{
    const double a = 0.17883277;
    const double b = 1.0 - 4.0 * a;
    const double c = 0.5 - a * log(4.0 * a);
    if (V <= 0.5) return (V * V) / 3.0;
    return (exp((V - c) / a) + b) / 12.0;
}

/* sRGB / BT.709 EOTF for SDR sources (simplified BT.1886, gamma 2.4). */
static double sdr_eotf(double V)
{
    if (V <= 0) return 0;
    return 100.0 * pow(V, 2.4);  /* SDR reference white = 100 nits */
}

/* Nominal display peak (L_W) to assume when converting HLG scene light
 * to display light. HLG is display-referred only once you commit to a
 * mastering peak, so this is an assumption, not a measurement — take
 * the file's mastering-display claim when it carries one, else the
 * BT.2100 reference of 1000 nits. Callers surface which was used. */
static double g_hlg_peak_override = 0.0;

void probe_set_hlg_peak_override(double nits)
{
    g_hlg_peak_override = (nits > 0.0) ? nits : 0.0;
}

double probe_hlg_peak_nits(const AVFrame *frame)
{
    if (g_hlg_peak_override > 0.0) return g_hlg_peak_override;
    if (frame) {
        const AVFrameSideData *sd = av_frame_get_side_data(frame,
            AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        if (sd && sd->size >= sizeof(AVMasteringDisplayMetadata)) {
            const AVMasteringDisplayMetadata *m =
                (const AVMasteringDisplayMetadata *)sd->data;
            if (m->has_luminance && m->max_luminance.den) {
                double lw = av_q2d(m->max_luminance);
                if (lw > 0.0) return lw;
            }
        }
    }
    return 1000.0;
}

/* BT.2100 HLG system gamma. 1.2 at the 1000-nit reference peak. */
static double hlg_system_gamma(double lw)
{
    if (lw <= 0.0) lw = 1000.0;
    return 1.2 + 0.42 * log10(lw / 1000.0);
}

/* BT.2100 HLG OOTF, luma form: scene linear [0,1] -> display cd/m².
 *
 * Without this step hlg_inverse_oetf's output (which tops out at 1.0)
 * would be reported directly as nits, so every HLG clip read as a
 * ~1-nit source: peak, dynamic range and the "above 500 nits" figure
 * were all meaningless. */
static double hlg_display_nits(double scene_linear, double lw)
{
    if (scene_linear <= 0.0) return 0.0;
    return lw * pow(scene_linear, hlg_system_gamma(lw));
}

/* ------------------------------------------------------------------ */
LumReference lum_reference_of(enum AVColorTransferCharacteristic trc)
{
    switch (trc) {
    case AVCOL_TRC_SMPTE2084:    return LUM_ABSOLUTE_PQ;
    case AVCOL_TRC_ARIB_STD_B67: return LUM_HLG_OOTF;
    default:                     return LUM_SDR_RELATIVE;
    }
}

const char *lum_reference_name(LumReference r)
{
    switch (r) {
    case LUM_ABSOLUTE_PQ: return "PQ absolute";
    case LUM_HLG_OOTF:    return "HLG (assumed L_W)";
    default:              return "SDR relative (100-nit white)";
    }
}

bool lum_reference_is_absolute(LumReference r)
{
    return r == LUM_ABSOLUTE_PQ || r == LUM_HLG_OOTF;
}

/* ------------------------------------------------------------------ */
/* YUV → RGB matrix coefficients for common color spaces.             */
/* All values produce R, G, B in the same [0,1] range as Y'.          */
/* ------------------------------------------------------------------ */
typedef struct { double kr, kg, kb; } Matrix;
/* For Y' = kr*R' + kg*G' + kb*B':
 *   R' = Y' + 2(1-kr) * Cr'
 *   B' = Y' + 2(1-kb) * Cb'
 *   G' = (Y' - kr*R' - kb*B') / kg
 */

static Matrix matrix_for(enum AVColorSpace cs)
{
    switch (cs) {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return (Matrix){ 0.2627, 0.6780, 0.0593 };
    case AVCOL_SPC_BT709:
        return (Matrix){ 0.2126, 0.7152, 0.0722 };
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
        return (Matrix){ 0.299,  0.587,  0.114  };
    default:
        return (Matrix){ 0.2627, 0.6780, 0.0593 };  /* default to BT.2020 */
    }
}

/* ------------------------------------------------------------------ */
/* Histogram plumbing                                                  */
/* ------------------------------------------------------------------ */
static double bin_log2(int bin)
{
    return PROBE_HIST_MIN_LOG2 + ((double)bin + 0.5) / PROBE_HIST_PER_STOP;
}

int probe_nits_to_bin(double nits)
{
    if (!(nits > 0.0)) return -1;
    double b = (log2(nits) - PROBE_HIST_MIN_LOG2) * PROBE_HIST_PER_STOP;
    if (b < 0.0) return -1;                 /* underflow, not bin 0 */
    int i = (int)b;
    if (i >= PROBE_HIST_BINS) i = PROBE_HIST_BINS - 1;
    return i;
}

double probe_bin_to_nits(int bin)
{
    if (bin < 0 || bin >= PROBE_HIST_BINS) return NAN;
    return exp2(bin_log2(bin));
}

static inline void hist_add(ProbeHist *h, int bin, bool is_black)
{
    if (is_black)  { h->black++;     return; }
    if (bin < 0)   { h->underflow++; return; }
    h->bins[bin]++;
    h->valid++;
}

double probe_hist_percentile(const ProbeHist *h, double pct)
{
    if (!h || h->valid == 0) return NAN;
    if (pct <= 0.0)   pct = 0.0;
    if (pct >= 100.0) pct = 100.0;

    double target = pct / 100.0 * (double)h->valid;
    uint64_t acc = 0;
    for (int i = 0; i < PROBE_HIST_BINS; i++) {
        acc += h->bins[i];
        if ((double)acc >= target && h->bins[i]) return probe_bin_to_nits(i);
    }
    /* Only reachable for pct == 100 with trailing empty bins. */
    for (int i = PROBE_HIST_BINS - 1; i >= 0; i--)
        if (h->bins[i]) return probe_bin_to_nits(i);
    return NAN;
}

double probe_hist_mean_log2(const ProbeHist *h)
{
    if (!h || h->valid == 0) return NAN;
    double sum = 0.0;
    for (int i = 0; i < PROBE_HIST_BINS; i++)
        if (h->bins[i]) sum += (double)h->bins[i] * bin_log2(i);
    return sum / (double)h->valid;
}

double probe_hist_var_log2(const ProbeHist *h)
{
    if (!h || h->valid == 0) return NAN;
    double mean = probe_hist_mean_log2(h);
    double sum = 0.0;
    for (int i = 0; i < PROBE_HIST_BINS; i++) {
        if (!h->bins[i]) continue;
        double d = bin_log2(i) - mean;
        sum += (double)h->bins[i] * d * d;
    }
    return sum / (double)h->valid;
}

/* ------------------------------------------------------------------ */
/* Transfer lookup tables.                                             */
/*                                                                     */
/* Two tables, both rebuilt only when the frame's coding parameters    */
/* change, and both justified by the same observation: the transfer is */
/* a pure function of its input, and the input is quantized.           */
/*                                                                     */
/*   LumaLut — indexed directly by the integer y_raw. Exact, no        */
/*             interpolation, at most 4096 entries at 12-bit. This is  */
/*             what makes stride-1 offline scans affordable: it        */
/*             replaces two pow() calls per sample (plus, for HLG, a   */
/*             third) with one array read.                             */
/*                                                                     */
/*   SigLut  — indexed by a normalized signal value in [0,1], with     */
/*             linear interpolation. Needed for the per-channel RGB    */
/*             path, where R'G'B' come out of the YUV matrix as        */
/*             continuous values rather than codes. 4096 entries puts  */
/*             interpolation error far below the sampling error the    */
/*             statistics already carry.                               */
/*                                                                     */
/* Not thread-safe: both caches are file-static singletons, matching   */
/* the single-threaded decode loop they serve.                         */
/* ------------------------------------------------------------------ */
#define SIG_LUT_N 4096

typedef struct {
    bool   valid;
    int    depth;
    bool   full_range;
    enum AVColorTransferCharacteristic trc;
    double hlg_lw;

    int    n;                       /* 1 << depth                     */
    int    y_lo;                    /* code-domain black threshold    */
    float  nits[1 << 12];
    int16_t bin[1 << 12];
} LumaLut;

typedef struct {
    bool   valid;
    enum AVColorTransferCharacteristic trc;
    /* For PQ/SDR this holds nits. For HLG it holds SCENE linear — the
     * OOTF depends on scene luma across all three channels, so it is
     * applied by the caller after the three lookups. */
    float  v[SIG_LUT_N];
} SigLut;

static LumaLut g_luma_lut;
static SigLut  g_sig_lut;

static const LumaLut *luma_lut_get(int depth, bool full_range,
                                   enum AVColorTransferCharacteristic trc,
                                   double hlg_lw)
{
    LumaLut *L = &g_luma_lut;
    if (L->valid && L->depth == depth && L->full_range == full_range &&
        L->trc == trc && L->hlg_lw == hlg_lw)
        return L;

    L->depth = depth; L->full_range = full_range;
    L->trc = trc; L->hlg_lw = hlg_lw;
    L->n = 1 << depth;

    int max_raw = L->n - 1;
    int y_lo = 16  << (depth - 8);
    int y_hi = 235 << (depth - 8);
    L->y_lo = full_range ? 0 : y_lo;

    bool is_hlg = (trc == AVCOL_TRC_ARIB_STD_B67);
    double (*eotf)(double) =
        (trc == AVCOL_TRC_SMPTE2084) ? pq_eotf :
        is_hlg                       ? hlg_inverse_oetf : sdr_eotf;

    for (int raw = 0; raw < L->n; raw++) {
        double Y = full_range ? (double)raw / max_raw
                              : (double)(raw - y_lo) / (double)(y_hi - y_lo);
        if (Y < 0) Y = 0;
        if (Y > 1) Y = 1;
        double nits = eotf(Y);
        if (is_hlg) nits = hlg_display_nits(nits, hlg_lw);
        L->nits[raw] = (float)nits;
        L->bin[raw]  = (int16_t)probe_nits_to_bin(nits);
    }
    L->valid = true;
    return L;
}

static const SigLut *sig_lut_get(enum AVColorTransferCharacteristic trc)
{
    SigLut *S = &g_sig_lut;
    if (S->valid && S->trc == trc) return S;

    S->trc = trc;
    double (*eotf)(double) =
        (trc == AVCOL_TRC_SMPTE2084)    ? pq_eotf :
        (trc == AVCOL_TRC_ARIB_STD_B67) ? hlg_inverse_oetf : sdr_eotf;

    for (int i = 0; i < SIG_LUT_N; i++)
        S->v[i] = (float)eotf((double)i / (SIG_LUT_N - 1));
    S->valid = true;
    return S;
}

static inline double sig_lut_eval(const SigLut *S, double V)
{
    if (V <= 0.0) return S->v[0];
    if (V >= 1.0) return S->v[SIG_LUT_N - 1];
    double f = V * (SIG_LUT_N - 1);
    int    i = (int)f;
    double t = f - i;
    return S->v[i] * (1.0 - t) + S->v[i + 1] * t;
}

/* ------------------------------------------------------------------ */
/* Pixel-format guard.                                                 */
/*                                                                     */
/* Both readers below index the planes as tightly packed native-endian */
/* uint8/uint16 arrays. That is only valid for planar, little-endian,  */
/* non-padded YUV. Checking comp[0].depth alone is NOT enough: P010    */
/* reports depth 10 but stores those bits in the HIGH end of a 16-bit  */
/* word (shift == 6) with 2-byte steps, so dividing by (1<<10)-1       */
/* overshoots ~64x and every sample saturates to the transfer's        */
/* maximum — 10000 nits for PQ. P010 is a routine hardware-decoder     */
/* output, so this silently poisoned the readout for a whole class of  */
/* inputs.                                                             */
/*                                                                     */
/* The luma and chroma requirements are deliberately separate. NV12's  */
/* Y plane is an ordinary 8-bit planar array and yields perfectly good */
/* luma statistics; only its interleaved UV plane is unreadable here.  */
/* Gating both on one predicate would throw away frame stats for a     */
/* common hardware-decoder format for no reason.                       */
static bool luma_plane_supported(const AVPixFmtDescriptor *desc, int *depth_out)
{
    if (!desc || desc->nb_components < 1) return false;
    if (desc->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL |
                       AV_PIX_FMT_FLAG_HWACCEL   | AV_PIX_FMT_FLAG_RGB |
                       AV_PIX_FMT_FLAG_BE))
        return false;
    if (!(desc->flags & AV_PIX_FMT_FLAG_PLANAR)) return false;

    int depth = desc->comp[0].depth;
    if (depth != 8 && depth != 10 && depth != 12) return false;

    /* No bit offset within the word, and one sample per word. This is
     * what excludes P010: depth 10, but shift 6 and a 2-byte step over
     * a 16-bit word. (Reading it correctly would just mean a >> 6;
     * left as a follow-up rather than smuggled into a bug fix.) */
    if (desc->comp[0].shift != 0) return false;
    if (desc->comp[0].step != (depth == 8 ? 1 : 2)) return false;
    if (desc->comp[0].offset != 0) return false;

    if (depth_out) *depth_out = depth;
    return true;
}

/* Cb and Cr each in their own plane, one sample per word, same depth
 * as luma. Excludes semi-planar layouts (NV12/NV21/P0xx) where both
 * chroma components share one interleaved plane. */
static bool chroma_planes_supported(const AVPixFmtDescriptor *desc)
{
    if (!desc || desc->nb_components < 3) return false;
    int depth = desc->comp[0].depth;
    for (int c = 1; c <= 2; c++) {
        if (desc->comp[c].plane  != c) return false;
        if (desc->comp[c].depth  != depth) return false;
        if (desc->comp[c].shift  != 0) return false;
        if (desc->comp[c].offset != 0) return false;
        if (desc->comp[c].step   != (depth == 8 ? 1 : 2)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* BT.2020 -> BT.709 linear conversion, used only to decide whether a  */
/* pixel's chromaticity falls outside the 709 gamut. Negative output   */
/* components mean the colour is unreachable in 709.                   */
static bool outside_bt709(double r, double g, double b)
{
    double r7 =  1.6605 * r - 0.5876 * g - 0.0728 * b;
    double g7 = -0.1246 * r + 1.1329 * g - 0.0083 * b;
    double b7 = -0.0182 * r - 0.1006 * g + 1.1187 * b;
    double mx = fmax(r, fmax(g, b));
    if (mx <= 0.0) return false;
    double tol = -1e-3 * mx;      /* scale-relative, ignores rounding */
    return r7 < tol || g7 < tol || b7 < tol;
}

/* ------------------------------------------------------------------ */
bool probe_sample(const AVFrame *frame, int src_x, int src_y, ProbeResult *out)
{
    memset(out, 0, sizeof(*out));
    if (!frame || !frame->data[0]) return false;

    /* Clamp into frame bounds. */
    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x >= frame->width)  src_x = frame->width  - 1;
    if (src_y >= frame->height) src_y = frame->height - 1;
    out->src_x = src_x;
    out->src_y = src_y;
    out->reference = lum_reference_of(frame->color_trc);

    /* Inspect pix_fmt — 8/10/12-bit planar YUV, any chroma layout. */
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int depth = 0;
    if (!luma_plane_supported(desc, &depth)) return false;
    if (!chroma_planes_supported(desc)) return false;
    if (!frame->data[1] || !frame->data[2]) return false;

    /* Raw sample values. Limited range black/white differ by depth.   */
    /* Chroma siting comes from the descriptor rather than a hardcoded */
    /* 4:2:0 halving — 4:2:2 has full vertical chroma and 4:4:4 has no */
    /* subsampling at all, so the old `cy = src_y / 2` read the wrong  */
    /* row for both. Those are ordinary mastering formats here.        */
    int y_raw, u_raw, v_raw;
    int cx = src_x >> desc->log2_chroma_w;
    int cy = src_y >> desc->log2_chroma_h;

    if (depth == 8) {
        y_raw = frame->data[0][src_y * frame->linesize[0] + src_x];
        u_raw = frame->data[1][cy    * frame->linesize[1] + cx];
        v_raw = frame->data[2][cy    * frame->linesize[2] + cx];
    } else {
        /* 10/12-bit planar little-endian. */
        y_raw = ((uint16_t *)frame->data[0])[src_y * (frame->linesize[0] / 2) + src_x];
        u_raw = ((uint16_t *)frame->data[1])[cy    * (frame->linesize[1] / 2) + cx];
        v_raw = ((uint16_t *)frame->data[2])[cy    * (frame->linesize[2] / 2) + cx];
    }

    /* Normalize to [0,1] / [-0.5,0.5] honoring limited vs full range. */
    int max  = (1 << depth) - 1;
    double Y, Cb, Cr;
    if (frame->color_range == AVCOL_RANGE_JPEG) {  /* full range */
        Y  = (double)y_raw / max;
        Cb = (double)u_raw / max - 0.5;
        Cr = (double)v_raw / max - 0.5;
    } else {                                       /* limited (default) */
        int y_lo = 16  << (depth - 8);
        int y_hi = 235 << (depth - 8);
        int c_lo = 16  << (depth - 8);
        int c_hi = 240 << (depth - 8);
        int c_mid = (c_lo + c_hi) / 2;
        Y  = (double)(y_raw - y_lo) / (y_hi - y_lo);
        Cb = (double)(u_raw - c_mid) / (c_hi - c_lo);
        Cr = (double)(v_raw - c_mid) / (c_hi - c_lo);
    }
    out->y_norm = Y;

    /* YUV → RGB. */
    Matrix m = matrix_for(frame->colorspace);
    double R = Y + 2.0 * (1.0 - m.kr) * Cr;
    double B = Y + 2.0 * (1.0 - m.kb) * Cb;
    double G = (Y - m.kr * R - m.kb * B) / m.kg;
    if (R < 0) R = 0; if (R > 1) R = 1;
    if (G < 0) G = 0; if (G > 1) G = 1;
    if (B < 0) B = 0; if (B > 1) B = 1;
    out->r_norm = R;
    out->g_norm = G;
    out->b_norm = B;

    /* Apply source transfer to get linear cd/m². */
    if (frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        /* HLG needs the BT.2100 OOTF, which is not separable per
         * channel: the system gamma is driven by the *scene luma*, then
         * applied to each channel.
         *     Y_S    = kr·R_S + kg·G_S + kb·B_S
         *     F_D(C) = L_W · Y_S^(gamma-1) · C_S
         * Skipping it left the values scene-referred and capped at 1.0,
         * so probe readouts on HLG clips reported ~1 nit for full-scale
         * white. */
        double Rs = hlg_inverse_oetf(R);
        double Gs = hlg_inverse_oetf(G);
        double Bs = hlg_inverse_oetf(B);
        double lw = probe_hlg_peak_nits(frame);
        double Ys = m.kr * Rs + m.kg * Gs + m.kb * Bs;
        double scale = (Ys > 0.0)
            ? lw * pow(Ys, hlg_system_gamma(lw) - 1.0) : 0.0;
        out->r_nits = scale * Rs;
        out->g_nits = scale * Gs;
        out->b_nits = scale * Bs;
    } else {
        double (*eotf)(double) =
            (frame->color_trc == AVCOL_TRC_SMPTE2084) ? pq_eotf : sdr_eotf;
        out->r_nits = eotf(R);
        out->g_nits = eotf(G);
        out->b_nits = eotf(B);
    }
    out->luma_nits = m.kr * out->r_nits + m.kg * out->g_nits + m.kb * out->b_nits;

    return true;
}

/* ------------------------------------------------------------------ */
/* Per-frame brightness statistics.                                   */
/*                                                                    */
/* The luma pass walks the Y plane through the LUT. For the "how      */
/* bright is this frame" question Y' is accurate enough and much      */
/* cheaper than reconstructing RGB — but it is NOT what CTA-861.3     */
/* defines MaxCLL over, which is why PROBE_FULL_RGB exists.           */
/*                                                                    */
/* Note the residual bias in the luma path: the YUV matrix is applied */
/* to *nonlinear* R'G'B', and all three EOTFs are convex on [0,1], so */
/* by Jensen's inequality eotf(Y') <= the true luminance. Combined    */
/* with sparse sampling this makes every luma-derived peak a one-     */
/* sided LOWER bound. Callers must not treat "measured <= declared"   */
/* as confirmation of anything.                                       */
/* ------------------------------------------------------------------ */
bool probe_frame_stats(const AVFrame *frame, int sample_stride,
                       ProbeMode mode, FrameStats *out)
{
    memset(out, 0, sizeof(*out));
    if (!frame || !frame->data[0]) return false;
    if (sample_stride < 1) sample_stride = 1;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int depth = 0;
    if (!luma_plane_supported(desc, &depth)) return false;

    bool want_rgb = (mode == PROBE_FULL_RGB);
    if (want_rgb && (!chroma_planes_supported(desc) ||
                     !frame->data[1] || !frame->data[2]))
        return false;

    bool  full_range = (frame->color_range == AVCOL_RANGE_JPEG);
    out->reference   = lum_reference_of(frame->color_trc);
    out->hlg_lw      = (out->reference == LUM_HLG_OOTF)
                       ? probe_hlg_peak_nits(frame) : 0.0;
    out->width       = frame->width;
    out->height      = frame->height;
    out->pix_fmt     = frame->format;
    out->color_trc   = frame->color_trc;
    out->color_range = frame->color_range;

    const LumaLut *L = luma_lut_get(depth, full_range, frame->color_trc,
                                    out->hlg_lw);

    /* Per-channel scaffolding, only touched in full-RGB mode. */
    const SigLut *S      = want_rgb ? sig_lut_get(frame->color_trc) : NULL;
    Matrix        m      = matrix_for(frame->colorspace);
    bool          is_hlg = (out->reference == LUM_HLG_OOTF);
    double        hlg_g1 = is_hlg ? hlg_system_gamma(out->hlg_lw) - 1.0 : 0.0;
    bool          wide   = (frame->color_primaries == AVCOL_PRI_BT2020);
    int           c_lo = 16 << (depth - 8), c_hi = 240 << (depth - 8);
    int           c_mid = (c_lo + c_hi) / 2;
    int           max_raw = (1 << depth) - 1;

    int w = frame->width, h = frame->height;
    int y_stride_pix = (depth == 8) ? frame->linesize[0] : (frame->linesize[0] / 2);

    double peak = 0.0, floor_min = 1e9, sum = 0.0;
    double rgb_peak = 0.0, rgb_sum = 0.0;
    int    n = 0, a100 = 0, a500 = 0, a1000 = 0, out709 = 0;

    /* Legacy floor threshold, kept only to populate floor_nits for the
     * existing HUD line. The dynamic-range figure below comes from the
     * histogram instead, which is what makes it outlier-resistant. */
    const double floor_cutoff_nits = 0.05;

    for (int y = 0; y < h; y += sample_stride) {
        const uint8_t  *row8  = frame->data[0] + (size_t)y * frame->linesize[0];
        const uint16_t *row16 = (const uint16_t *)frame->data[0] + (size_t)y * y_stride_pix;

        for (int x = 0; x < w; x += sample_stride) {
            int yraw = (depth == 8) ? row8[x] : row16[x];
            if (yraw < 0) yraw = 0;
            if (yraw > max_raw) yraw = max_raw;

            double nits    = L->nits[yraw];
            bool   isblack = (yraw <= L->y_lo);

            hist_add(&out->luma, L->bin[yraw], isblack);

            if (nits > peak) peak = nits;
            if (nits > floor_cutoff_nits && nits < floor_min) floor_min = nits;
            sum += nits;
            n++;
            if (nits > 100.0)  a100++;
            if (nits > 500.0)  a500++;
            if (nits > 1000.0) a1000++;

            if (!want_rgb) continue;

            /* --- per-channel path ------------------------------- */
            int cx = x >> desc->log2_chroma_w;
            int cy = y >> desc->log2_chroma_h;
            int u_raw, v_raw;
            if (depth == 8) {
                u_raw = frame->data[1][cy * frame->linesize[1] + cx];
                v_raw = frame->data[2][cy * frame->linesize[2] + cx];
            } else {
                u_raw = ((const uint16_t *)frame->data[1])[cy * (frame->linesize[1] / 2) + cx];
                v_raw = ((const uint16_t *)frame->data[2])[cy * (frame->linesize[2] / 2) + cx];
            }

            double Yn, Cb, Cr;
            if (full_range) {
                Yn = (double)yraw / max_raw;
                Cb = (double)u_raw / max_raw - 0.5;
                Cr = (double)v_raw / max_raw - 0.5;
            } else {
                int y_lo = 16 << (depth - 8), y_hi = 235 << (depth - 8);
                Yn = (double)(yraw - y_lo) / (y_hi - y_lo);
                Cb = (double)(u_raw - c_mid) / (c_hi - c_lo);
                Cr = (double)(v_raw - c_mid) / (c_hi - c_lo);
            }
            if (Yn < 0) Yn = 0; if (Yn > 1) Yn = 1;

            double R = Yn + 2.0 * (1.0 - m.kr) * Cr;
            double B = Yn + 2.0 * (1.0 - m.kb) * Cb;
            double G = (Yn - m.kr * R - m.kb * B) / m.kg;
            if (R < 0) R = 0; if (R > 1) R = 1;
            if (G < 0) G = 0; if (G > 1) G = 1;
            if (B < 0) B = 0; if (B > 1) B = 1;

            double rl = sig_lut_eval(S, R);
            double gl = sig_lut_eval(S, G);
            double bl = sig_lut_eval(S, B);
            if (is_hlg) {
                /* One common scale across channels, so channel ORDER is
                 * unchanged by the OOTF — max() commutes with it. */
                double Ys = m.kr * rl + m.kg * gl + m.kb * bl;
                double s  = (Ys > 0.0) ? out->hlg_lw * pow(Ys, hlg_g1) : 0.0;
                rl *= s; gl *= s; bl *= s;
            }

            double mx = fmax(rl, fmax(gl, bl));
            hist_add(&out->maxrgb, probe_nits_to_bin(mx), isblack);
            if (mx > rgb_peak) rgb_peak = mx;
            rgb_sum += mx;
            if (wide && outside_bt709(rl, gl, bl)) out709++;
        }
    }

    if (n == 0) return false;

    out->peak_nits      = peak;
    out->avg_nits       = sum / n;
    out->floor_nits     = (floor_min < 1e9) ? floor_min : 0.0;
    out->pct_above_100  = 100.0f * a100  / n;
    out->pct_above_500  = 100.0f * a500  / n;
    out->pct_above_1000 = 100.0f * a1000 / n;
    out->samples        = n;

    out->mu_log2  = probe_hist_mean_log2(&out->luma);
    out->var_log2 = probe_hist_var_log2(&out->luma);

    /* Outlier-resistant dynamic range. p1 rather than p0.1 for the
     * floor: the bottom fraction of a dark HDR frame is codec noise
     * numbering in the thousands, not a handful of stray samples, so
     * p0.1 does not escape it. */
    double lo = probe_hist_percentile(&out->luma, 1.0);
    double hi = probe_hist_percentile(&out->luma, 99.9);
    out->dr_stops = (isfinite(lo) && isfinite(hi) && lo > 0.0 && hi > 0.0)
                    ? log2(hi / lo) : 0.0;

    if (want_rgb) {
        out->has_maxrgb      = true;
        out->maxrgb_peak     = rgb_peak;
        out->maxrgb_avg      = rgb_sum / n;
        out->pct_outside_709 = wide ? (100.0f * out709 / n) : 0.0f;
    }
    return true;
}
