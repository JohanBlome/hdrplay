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

/* BT.1886 EOTF for SDR sources. Gamma 2.4, 100-nit reference white,
 * lifted by the display black level L_B:
 *
 *     L = a·max(V + b, 0)^2.4
 *     a = (L_W^(1/2.4) - L_B^(1/2.4))^2.4
 *     b = L_B^(1/2.4) / (L_W^(1/2.4) - L_B^(1/2.4))
 *
 * At L_B = 0 this collapses to the bare 100·V^2.4 the code used to
 * assume unconditionally. That form has no floor, so the darkest
 * non-black code maps arbitrarily close to zero and every ratio taken
 * against it — the dynamic-range figure above all — reports the code
 * lattice instead of the picture. See probe_set_sdr_black_nits(). */
#define SDR_GAMMA 2.4
#define SDR_LW    100.0

static double g_sdr_black_nits = 0.1;   /* BT.1886 reference, 1000:1 */
static double g_sdr_a = SDR_LW, g_sdr_b = 0.0;
static bool   g_sdr_coeffs_valid = false;

/* a and b are transcendental in L_B, so they cannot be static
 * initializers; resolve them on first use and on every override. */
static void sdr_coeffs_ensure(void)
{
    if (g_sdr_coeffs_valid) return;
    g_sdr_coeffs_valid = true;

    double lb = g_sdr_black_nits;
    if (!(lb > 0.0)) { g_sdr_a = SDR_LW; g_sdr_b = 0.0; return; }
    double lw_r = pow(SDR_LW, 1.0 / SDR_GAMMA);
    double lb_r = pow(lb,     1.0 / SDR_GAMMA);
    double den  = lw_r - lb_r;
    if (!(den > 0.0)) { g_sdr_a = SDR_LW; g_sdr_b = 0.0; return; }
    g_sdr_a = pow(den, SDR_GAMMA);
    g_sdr_b = lb_r / den;
}

static double sdr_eotf(double V)
{
    sdr_coeffs_ensure();
    double x = V + g_sdr_b;
    if (x <= 0.0) return 0.0;
    return g_sdr_a * pow(x, SDR_GAMMA);
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

double probe_hlg_peak_override_nits(void)
{
    return g_hlg_peak_override;
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

/* ------------------------------------------------------------------ */
void probe_set_sdr_black_nits(double nits)
{
    g_sdr_black_nits    = (nits > 0.0) ? nits : 0.0;
    g_sdr_coeffs_valid  = false;
    /* Both tables bake the transfer in, and neither keys on the black
     * level — invalidate rather than widen the cache key for a value
     * that changes once, at startup. */
    g_luma_lut.valid = false;
    g_sig_lut.valid  = false;
}

double probe_sdr_black_nits(void)
{
    return g_sdr_black_nits;
}

double probe_dr_ceiling_stops(enum AVColorTransferCharacteristic trc,
                              int depth, bool full_range, double hlg_lw)
{
    if (depth < 8)  depth = 8;
    if (depth > 12) depth = 12;

    int max_raw = (1 << depth) - 1;
    int y_lo = full_range ? 0       : (16  << (depth - 8));
    int y_hi = full_range ? max_raw : (235 << (depth - 8));
    if (y_hi <= y_lo) return 0.0;

    bool is_hlg = (trc == AVCOL_TRC_ARIB_STD_B67);
    double (*eotf)(double) =
        (trc == AVCOL_TRC_SMPTE2084) ? pq_eotf :
        is_hlg                       ? hlg_inverse_oetf : sdr_eotf;

    /* One code above code-domain black is the darkest value the coding
     * can carry that the probe will admit to the percentile
     * population. Everything below is counted as black and excluded. */
    double darkest = eotf(1.0 / (double)(y_hi - y_lo));
    double white   = eotf(1.0);
    if (is_hlg) {
        darkest = hlg_display_nits(darkest, hlg_lw);
        white   = hlg_display_nits(white,   hlg_lw);
    }
    if (!(darkest > 0.0) || !(white > 0.0)) return INFINITY;
    return log2(white / darkest);
}

/* Defined with the other pixel-format guards below. */
static bool luma_plane_supported(const AVPixFmtDescriptor *desc, int *depth_out);
static inline int read_component(const AVFrame *frame,
                                 const AVPixFmtDescriptor *desc,
                                 int c, int x, int y);

enum AVColorRange probe_guess_color_range(const AVFrame *frame,
                                          double *out_frac)
{
    if (out_frac) *out_frac = 0.0;
    if (!frame || !frame->data[0]) return AVCOL_RANGE_UNSPECIFIED;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int depth = 0;
    if (!luma_plane_supported(desc, &depth)) return AVCOL_RANGE_UNSPECIFIED;

    int scale = 1 << (depth - 8);
    int guard = PROBE_RANGE_GUARD_CODES * scale;
    int lo    = 16  * scale - guard;
    int hi    = 235 * scale + guard;

    int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0) return AVCOL_RANGE_UNSPECIFIED;
    /* Striding is fine here: this is a population fraction, not a peak,
     * and the threshold is three orders of magnitude above the sampling
     * noise of a 4-in-both-axes stride on any real frame size. */
    const int stride = (w >= 256 && h >= 256) ? 4 : 1;
    uint64_t outside = 0, total = 0;
    for (int y = 0; y < h; y += stride) {
        for (int x = 0; x < w; x += stride) {
            int v = read_component(frame, desc, 0, x, y);
            if (v < lo || v > hi) outside++;
            total++;
        }
    }
    if (!total) return AVCOL_RANGE_UNSPECIFIED;

    double frac = (double)outside / (double)total;
    if (out_frac) *out_frac = frac;
    return (frac > PROBE_RANGE_FULL_FRAC) ? AVCOL_RANGE_JPEG
                                          : AVCOL_RANGE_MPEG;
}

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
/* Read through AVComponentDescriptor rather than assuming one tightly */
/* packed plane per component. Hardware decoders normally download as  */
/* NV12/P010: chroma is interleaved and P010 stores its 10-bit values in */
/* the high bits of 16-bit words. The descriptor's plane/step/offset/   */
/* shift fields describe both without pixel-format special cases.      */
static bool component_supported(const AVPixFmtDescriptor *desc, int c)
{
    if (!desc || c < 0 || c >= desc->nb_components) return false;
    const AVComponentDescriptor *cd = &desc->comp[c];
    int bytes = (cd->depth + cd->shift + 7) / 8;
    return cd->plane >= 0 && cd->plane < 4 &&
           cd->depth > 0 && cd->depth <= 16 &&
           cd->shift >= 0 && bytes > 0 &&
           cd->step > 0 && cd->offset >= 0 &&
           cd->offset + bytes <= cd->step;
}

static inline int read_component(const AVFrame *frame,
                                 const AVPixFmtDescriptor *desc,
                                 int c, int x, int y)
{
    const AVComponentDescriptor *cd = &desc->comp[c];
    if (c == 1 || c == 2) {
        x >>= desc->log2_chroma_w;
        y >>= desc->log2_chroma_h;
    }
    const uint8_t *p = frame->data[cd->plane] +
                       (size_t)y * frame->linesize[cd->plane] +
                       (size_t)x * cd->step + cd->offset;
    int bytes = (cd->depth + cd->shift + 7) / 8;
    unsigned raw = 0;
    for (int i = 0; i < bytes; i++) raw |= (unsigned)p[i] << (8 * i);
    return (int)((raw >> cd->shift) & ((1u << cd->depth) - 1u));
}

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
    if (!component_supported(desc, 0)) return false;

    if (depth_out) *depth_out = depth;
    return true;
}

static bool chroma_planes_supported(const AVPixFmtDescriptor *desc)
{
    if (!desc || desc->nb_components < 3) return false;
    int depth = desc->comp[0].depth;
    for (int c = 1; c <= 2; c++) {
        if (desc->comp[c].depth  != depth) return false;
        if (!component_supported(desc, c)) return false;
    }
    return true;
}

bool probe_rgb_waveform(const AVFrame *frame, int width, int height,
                        int vertical_stride, uint32_t *bins,
                        uint32_t peak_count[3], int *out_samples)
{
    if (peak_count) memset(peak_count, 0, 3 * sizeof(*peak_count));
    if (out_samples) *out_samples = 0;
    if (!frame || !frame->data[0] || !bins || width <= 0 || height <= 0)
        return false;
    if (vertical_stride < 1) vertical_stride = 1;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int depth = 0;
    if (!luma_plane_supported(desc, &depth) ||
        !chroma_planes_supported(desc))
        return false;
    for (int c = 0; c < 3; c++)
        if (!frame->data[desc->comp[c].plane]) return false;

    size_t plane_size = (size_t)width * (size_t)height;
    memset(bins, 0, 3 * plane_size * sizeof(*bins));

    bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
    int max_raw = (1 << depth) - 1;
    int y_lo = 16 << (depth - 8), y_hi = 235 << (depth - 8);
    int c_lo = 16 << (depth - 8), c_hi = 240 << (depth - 8);
    int c_mid = (c_lo + c_hi) / 2;
    Matrix m = matrix_for(frame->colorspace);
    const double signal_span = PROBE_WAVEFORM_MAX_SIGNAL -
                               PROBE_WAVEFORM_MIN_SIGNAL;
    uint32_t local_peak[3] = {0};
    int samples = 0;

    /* Iterate over graph columns rather than sparse source X values. This
     * guarantees a continuous scope even when a low-resolution input is
     * displayed in a wide waveform panel. */
    for (int bx = 0; bx < width; bx++) {
        int x = (int)(((int64_t)bx * 2 + 1) * frame->width /
                      ((int64_t)width * 2));
        if (x >= frame->width) x = frame->width - 1;
        for (int y = 0; y < frame->height; y += vertical_stride) {
            int y_raw = read_component(frame, desc, 0, x, y);
            int u_raw = read_component(frame, desc, 1, x, y);
            int v_raw = read_component(frame, desc, 2, x, y);

            double Yn, Cb, Cr;
            if (full_range) {
                Yn = (double)y_raw / max_raw;
                Cb = (double)u_raw / max_raw - 0.5;
                Cr = (double)v_raw / max_raw - 0.5;
            } else {
                Yn = (double)(y_raw - y_lo) / (y_hi - y_lo);
                Cb = (double)(u_raw - c_mid) / (c_hi - c_lo);
                Cr = (double)(v_raw - c_mid) / (c_hi - c_lo);
            }

            double rgb[3];
            rgb[0] = Yn + 2.0 * (1.0 - m.kr) * Cr;
            rgb[2] = Yn + 2.0 * (1.0 - m.kb) * Cb;
            rgb[1] = (Yn - m.kr * rgb[0] - m.kb * rgb[2]) / m.kg;

            for (int c = 0; c < 3; c++) {
                double t = (rgb[c] - PROBE_WAVEFORM_MIN_SIGNAL) /
                           signal_span;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                int row = height - 1 - (int)llround(t * (height - 1));
                size_t idx = ((size_t)c * height + row) * width + bx;
                uint32_t count = ++bins[idx];
                if (count > local_peak[c]) local_peak[c] = count;
            }
            samples++;
        }
    }

    if (peak_count) memcpy(peak_count, local_peak, sizeof(local_peak));
    if (out_samples) *out_samples = samples;
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

    /* Inspect pix_fmt — 8/10/12-bit planar or semi-planar YUV. */
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int depth = 0;
    if (!luma_plane_supported(desc, &depth)) return false;
    if (!chroma_planes_supported(desc)) return false;
    for (int c = 0; c < 3; c++)
        if (!frame->data[desc->comp[c].plane]) return false;

    /* Raw sample values. Limited range black/white differ by depth.   */
    /* Chroma siting comes from the descriptor rather than a hardcoded */
    /* 4:2:0 halving — 4:2:2 has full vertical chroma and 4:4:4 has no */
    /* subsampling at all, so the old `cy = src_y / 2` read the wrong  */
    /* row for both. Those are ordinary mastering formats here.        */
    int y_raw, u_raw, v_raw;
    y_raw = read_component(frame, desc, 0, src_x, src_y);
    u_raw = read_component(frame, desc, 1, src_x, src_y);
    v_raw = read_component(frame, desc, 2, src_x, src_y);

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
    if (want_rgb && !chroma_planes_supported(desc)) return false;
    int components = want_rgb ? 3 : 1;
    for (int c = 0; c < components; c++)
        if (!frame->data[desc->comp[c].plane]) return false;

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
    double peak = 0.0, floor_min = 1e9, sum = 0.0;
    double rgb_peak = 0.0, rgb_sum = 0.0;
    int    n = 0, a100 = 0, a500 = 0, a1000 = 0, out709 = 0;

    /* Legacy floor threshold, kept only to populate floor_nits for the
     * existing HUD line. The dynamic-range figure below comes from the
     * histogram instead, which is what makes it outlier-resistant. */
    const double floor_cutoff_nits = 0.05;

    for (int y = 0; y < h; y += sample_stride) {
        for (int x = 0; x < w; x += sample_stride) {
            int yraw = read_component(frame, desc, 0, x, y);
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
            int u_raw = read_component(frame, desc, 1, x, y);
            int v_raw = read_component(frame, desc, 2, x, y);

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
