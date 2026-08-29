#ifndef HDRPLAY_PROBE_H
#define HDRPLAY_PROBE_H

#include <stdbool.h>
#include <stdint.h>
#include <libavutil/frame.h>

/* ------------------------------------------------------------------ */
/* Luminance reference.                                                */
/*                                                                     */
/* The three transfer functions do NOT produce commensurable numbers,  */
/* so every statistic carries the reference it was measured against.   */
/* PQ is absolute by definition. HLG only becomes absolute once you    */
/* commit to a nominal display peak (see probe_hlg_peak_nits). SDR has */
/* no absolute meaning at all — its "nits" are relative to a 100-nit   */
/* reference white, so a measured peak can never exceed 100.           */
/*                                                                     */
/* Ratio statistics (dynamic range, spread — anything in stops) are    */
/* valid for all three. Absolute statistics (MaxCLL, MaxFALL, "above   */
/* 500 nits") are meaningful only for the first two.                   */
typedef enum {
    LUM_ABSOLUTE_PQ,     /* cd/m², absolute                            */
    LUM_HLG_OOTF,        /* cd/m², premised on a stated L_W            */
    LUM_SDR_RELATIVE,    /* nominal only, 100-nit reference white      */
} LumReference;

LumReference lum_reference_of(enum AVColorTransferCharacteristic trc);
const char  *lum_reference_name(LumReference r);
bool         lum_reference_is_absolute(LumReference r);

/* ------------------------------------------------------------------ */
/* Log-luminance histogram.                                            */
/*                                                                     */
/* 512 bins of 1/16 stop spanning 2^-16 .. 2^16 nits, which covers     */
/* PQ's full 0.0001–10000 range with 4.4% luminance resolution. That   */
/* keeps quantization error on a derived stops figure near ±0.06,      */
/* comfortably under the one decimal place we print.                   */
/*                                                                     */
/* Three populations are tracked separately, and the distinction       */
/* matters more than it looks:                                         */
/*                                                                     */
/*   black     — code-domain black (y_raw <= y_lo). Exact and depth-   */
/*               aware. Letterbox and pillarbox bars land here, so     */
/*               they can be reported rather than silently skewing     */
/*               the low percentiles.                                  */
/*   underflow — non-black but below the bottom bin. Kept out of the   */
/*               percentile population: bin 0 is a clamp bucket, so    */
/*               letting p1 land in it would produce a meaningless     */
/*               near-constant dynamic range on every clip — exactly   */
/*               the failure the histogram exists to avoid.            */
/*   valid     — everything else. All percentiles are over this.       */
#define PROBE_HIST_BINS        512
#define PROBE_HIST_MIN_LOG2    (-16.0)
#define PROBE_HIST_PER_STOP    16.0

typedef struct {
    uint32_t bins[PROBE_HIST_BINS];
    uint64_t black;
    uint64_t underflow;
    uint64_t valid;
} ProbeHist;

int    probe_nits_to_bin(double nits);
double probe_bin_to_nits(int bin);

/* All of these operate over the `valid` population only, and return
 * NAN when it is empty. `pct` is 0..100. */
double probe_hist_percentile(const ProbeHist *h, double pct);
double probe_hist_mean_log2 (const ProbeHist *h);
double probe_hist_var_log2  (const ProbeHist *h);

/* ------------------------------------------------------------------ */
/* Single-pixel probe.                                                 */
/*                                                                     */
/* All "nits" values are in the source's own reference (i.e. what the  */
/* pixel data SAYS the scene luminance is, before any rendering,       */
/* tone-map or gamut-map happens on the GPU). This is the ground truth */
/* of the file — useful for "is this red supposed to be visible?".     */
typedef struct {
    int    src_x, src_y;           /* source pixel actually read       */
    double y_norm;                 /* normalized Y' (0..1)             */
    double r_norm, g_norm, b_norm; /* normalized RGB after YUV→RGB     */
    double r_nits, g_nits, b_nits; /* linear cd/m² per channel         */
    double luma_nits;              /* BT.2020/709-weighted luma cd/m²  */
    LumReference reference;
} ProbeResult;

/* Sample (src_x, src_y) from `frame`. Supports 8/10/12-bit planar
 * little-endian YUV at any chroma subsampling (420/422/444), covering
 * ~all HDR10, HLG and SDR clips plus ProRes-style 4:2:2 / 4:4:4
 * masters. Returns false for anything else — notably semi-planar and
 * bit-shifted layouts (NV12, P010), which naive plane indexing cannot
 * read. */
bool probe_sample(const AVFrame *frame, int src_x, int src_y,
                  ProbeResult *out);

/* Nominal display peak (L_W, cd/m²) used to convert HLG scene light to
 * display light. HLG carries no absolute luminance of its own, so this
 * is an assumption: the frame's mastering-display max when it declares
 * one, else the BT.2100 reference of 1000. Exposed so callers can
 * report which value the numbers are premised on. Meaningless for
 * non-HLG sources. */
double probe_hlg_peak_nits(const AVFrame *frame);

/* Force the HLG display peak, overriding both the file's declared
 * mastering display and the 1000-nit default. Pass <= 0 to clear.
 * Wired to --hlg-peak. */
void probe_set_hlg_peak_override(double nits);

/* ------------------------------------------------------------------ */
/* Per-frame brightness statistics.                                    */
/*                                                                     */
/* Sparse-samples the frame, applies the source transfer, and reports  */
/* the frame's brightness distribution. Use this to answer "does this  */
/* file actually exercise HDR?" — if peak_nits stays below ~500 across  */
/* the whole file, the file is effectively SDR-bright and no amount of */
/* pipeline correctness will produce a visible HDR/SDR delta.          */
typedef enum {
    /* Luma plane only. ~3x cheaper, and enough for brightness and     */
    /* dynamic range. Cannot produce MaxCLL/MaxFALL, which CTA-861.3   */
    /* defines over max(R,G,B) rather than luma. Used for live HUD.    */
    PROBE_LUMA_ONLY,
    /* Reconstructs per-channel RGB so maxRGB statistics are available */
    /* to spec. Needs readable chroma planes. Used for offline scans.  */
    PROBE_FULL_RGB,
} ProbeMode;

typedef struct {
    LumReference reference;
    double hlg_lw;           /* L_W assumed, when reference is HLG     */

    /* Coding parameters this frame was measured under. Carried so the
     * session accumulator can detect a mid-stream change and refuse to
     * pool histograms that are not comparable. */
    int    width, height;
    int    pix_fmt;
    int    color_trc;
    int    color_range;

    double peak_nits;        /* max sampled luma, cd/m²                */
    double avg_nits;         /* mean sampled luma, cd/m²               */
    double floor_nits;       /* min sampled luma above near-black      */
    double dr_stops;         /* log2(p99.9 / p1), outlier-resistant    */
    float  pct_above_100;    /* % of samples > 100 nits (SDR diffuse)  */
    float  pct_above_500;    /* % of samples > 500 nits (EDR-boost)    */
    float  pct_above_1000;   /* % of samples > 1000 nits (HDR)         */
    int    samples;          /* how many pixels were inspected         */

    ProbeHist luma;
    double    mu_log2;       /* mean of log2(nits) over valid samples  */
    double    var_log2;      /* variance ditto — feeds spatial spread  */

    /* PROBE_FULL_RGB only. maxrgb_peak/avg are the per-frame inputs to
     * MaxCLL and MaxFALL respectively. */
    bool      has_maxrgb;
    ProbeHist maxrgb;
    double    maxrgb_peak;
    double    maxrgb_avg;
    /* % of samples outside the BT.709 gamut. Only meaningful when the
     * source is tagged wider than 709. */
    float     pct_outside_709;
} FrameStats;

/* Compute per-frame statistics. `sample_stride` controls sparseness:
 * 1 = every pixel (exact, offline), 8 = every 8th in both axes (~130k
 * samples on 4K, sub-ms). Returns false on unsupported pix_fmt — see
 * probe_sample() for the accepted set. PROBE_FULL_RGB additionally
 * requires separate chroma planes, so it rejects NV12 where
 * PROBE_LUMA_ONLY accepts it. */
bool probe_frame_stats(const AVFrame *frame, int sample_stride,
                       ProbeMode mode, FrameStats *out);

#endif
