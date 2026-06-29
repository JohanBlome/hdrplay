#ifndef HDRPLAY_PROBE_H
#define HDRPLAY_PROBE_H

#include <stdbool.h>
#include <libavutil/frame.h>

/* Result of sampling a single pixel from a source AVFrame. All "nits"
 * values are in cd/m² in the source's reference (i.e. what the pixel
 * data SAYS the scene luminance is, before any rendering / tone-map /
 * gamut-map happens on the GPU). This is the "ground truth" of the
 * file — useful for answering "is this red supposed to be visible?". */
typedef struct {
    int    src_x, src_y;         /* source pixel actually read */
    double y_norm;               /* normalized Y' (0..1)        */
    double r_norm, g_norm, b_norm; /* normalized RGB after YUV→RGB */
    double r_nits, g_nits, b_nits; /* linear cd/m² per channel    */
    double luma_nits;            /* BT.2020/709-weighted luma cd/m² */
} ProbeResult;

/* Sample (src_x, src_y) from `frame`. Only yuv420p, yuv420p10le, and
 * yuv420p12le are currently supported (covers ~all HDR10 and SDR clips
 * you'll throw at this tool). Returns false for other pixel formats. */
bool probe_sample(const AVFrame *frame, int src_x, int src_y,
                  ProbeResult *out);

/* ------------------------------------------------------------------ */
/* Per-frame source brightness statistics.                            */
/* Sparse-samples the AVFrame's luma plane (much cheaper than a full  */
/* scan), applies the source's transfer function, and reports the     */
/* current frame's dynamic range plus how much of it sits above SDR.  */
/* Use this to answer "does this file actually exercise HDR?" — if    */
/* peak_nits stays below ~500 across the whole file, the file is      */
/* effectively SDR-bright and no amount of pipeline correctness will  */
/* produce a visible HDR/SDR delta. */
typedef struct {
    double peak_nits;        /* max sampled luma, cd/m²                   */
    double avg_nits;         /* mean sampled luma, cd/m²                  */
    double floor_nits;       /* min sampled luma above near-black, cd/m²  */
    double dr_stops;         /* log2(peak / floor)                        */
    float  pct_above_100;    /* % of samples > 100 nits (SDR diffuse)     */
    float  pct_above_500;    /* % of samples > 500 nits (EDR-boost SDR)   */
    float  pct_above_1000;   /* % of samples > 1000 nits (HDR highlights) */
    int    samples;          /* how many pixels were actually inspected   */
} FrameStats;

/* Compute per-frame brightness statistics. `sample_stride` controls
 * sparseness: 1 = every pixel (expensive), 8 = every 8th in both axes
 * (~130k samples on 4K, sub-ms on modern CPUs). Returns false on
 * unsupported pix_fmt. */
bool probe_frame_stats(const AVFrame *frame, int sample_stride,
                       FrameStats *out);

#endif
