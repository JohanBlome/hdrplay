#include "probe.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
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

/* HLG OETF inverse (BT.2100), scene-referred. Returns scene linear,
 * not display nits — for display nits you'd apply the system gamma. */
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

    /* Inspect pix_fmt — handle 8/10/12-bit planar 4:2:0. */
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    if (!desc || desc->nb_components < 3) return false;
    int depth = desc->comp[0].depth;
    if (depth != 8 && depth != 10 && depth != 12) return false;

    /* Raw sample values. Limited range black/white differ by depth.   */
    int y_raw, u_raw, v_raw;
    int cx = src_x / 2, cy = src_y / 2;   /* 4:2:0 chroma subsample */

    if (depth == 8) {
        y_raw = frame->data[0][src_y * frame->linesize[0] + src_x];
        u_raw = frame->data[1][cy    * frame->linesize[1] + cx];
        v_raw = frame->data[2][cy    * frame->linesize[2] + cx];
    } else {
        /* 10/12-bit planar little-endian (yuv420p10le, yuv420p12le). */
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
    double (*eotf)(double);
    switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE2084:                 eotf = pq_eotf;          break;
    case AVCOL_TRC_ARIB_STD_B67:              eotf = hlg_inverse_oetf; break;
    default:                                  eotf = sdr_eotf;         break;
    }
    out->r_nits = eotf(R);
    out->g_nits = eotf(G);
    out->b_nits = eotf(B);
    out->luma_nits = m.kr * out->r_nits + m.kg * out->g_nits + m.kb * out->b_nits;

    return true;
}

/* ------------------------------------------------------------------ */
/* Per-frame brightness statistics.                                   */
/*                                                                    */
/* Walks the luma plane with a stride, applies the source transfer    */
/* function to each sample, and accumulates peak/floor/percentile     */
/* counters. We use Y' (the gamma-corrected luma component) directly  */
/* rather than reconstructing per-channel RGB and computing luma from */
/* that — for the "how bright is this frame" question, Y' is accurate */
/* enough and ~3× cheaper. For PQ content specifically, pq_eotf(Y')   */
/* gives the absolute luma in nits because Y' encodes the luma signal */
/* through the same transfer that pq_eotf inverts.                    */
/* ------------------------------------------------------------------ */
bool probe_frame_stats(const AVFrame *frame, int sample_stride, FrameStats *out)
{
    memset(out, 0, sizeof(*out));
    if (!frame || !frame->data[0]) return false;
    if (sample_stride < 1) sample_stride = 1;

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    if (!desc || desc->nb_components < 1) return false;
    int depth = desc->comp[0].depth;
    if (depth != 8 && depth != 10 && depth != 12) return false;

    int   max_raw    = (1 << depth) - 1;
    int   y_lo       = 16  << (depth - 8);
    int   y_hi       = 235 << (depth - 8);
    bool  full_range = (frame->color_range == AVCOL_RANGE_JPEG);

    double (*eotf)(double);
    switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE2084:    eotf = pq_eotf;          break;
    case AVCOL_TRC_ARIB_STD_B67: eotf = hlg_inverse_oetf; break;
    default:                     eotf = sdr_eotf;         break;
    }

    int w = frame->width, h = frame->height;
    int stride_pix = (depth == 8) ? frame->linesize[0] : (frame->linesize[0] / 2);

    double peak  = 0.0;
    double floor_min = 1e9;
    double sum   = 0.0;
    int    n     = 0;
    int    a100 = 0, a500 = 0, a1000 = 0;

    /* Floor threshold: ignore samples below ~0.05 nits when computing
     * the dynamic-range floor — letterbox/pillarbox black bars (literal
     * zero) would otherwise pin the floor at zero and make dr_stops
     * blow up to infinity on every frame. */
    const double floor_cutoff_nits = 0.05;

    for (int y = 0; y < h; y += sample_stride) {
        for (int x = 0; x < w; x += sample_stride) {
            int yraw;
            if (depth == 8)
                yraw = frame->data[0][y * frame->linesize[0] + x];
            else
                yraw = ((uint16_t *)frame->data[0])[y * stride_pix + x];

            double Y;
            if (full_range) {
                Y = (double)yraw / max_raw;
            } else {
                Y = (double)(yraw - y_lo) / (double)(y_hi - y_lo);
            }
            if (Y < 0) Y = 0;
            if (Y > 1) Y = 1;

            double nits = eotf(Y);

            if (nits > peak) peak = nits;
            if (nits > floor_cutoff_nits && nits < floor_min) floor_min = nits;
            sum += nits;
            n++;
            if (nits > 100.0)  a100++;
            if (nits > 500.0)  a500++;
            if (nits > 1000.0) a1000++;
        }
    }

    if (n == 0) return false;

    out->peak_nits      = peak;
    out->avg_nits       = sum / n;
    out->floor_nits     = (floor_min < 1e9) ? floor_min : 0.0;
    out->dr_stops       = (peak > 0.0 && out->floor_nits > 0.0)
                          ? log2(peak / out->floor_nits) : 0.0;
    out->pct_above_100  = 100.0f * a100  / n;
    out->pct_above_500  = 100.0f * a500  / n;
    out->pct_above_1000 = 100.0f * a1000 / n;
    out->samples        = n;
    return true;
}
