#ifndef HDRPLAY_STATS_H
#define HDRPLAY_STATS_H

#include <stdbool.h>
#include <stdint.h>

#include "probe.h"

/* ------------------------------------------------------------------ */
/* Session-wide accumulation of per-frame statistics.                  */
/*                                                                     */
/* Answers the question a single frame cannot: "is this FILE worth     */
/* using as an HDR test clip?". Per-frame numbers flicker and are      */
/* discarded; these persist across playback, survive seeking, and feed */
/* both the live HUD panel and the offline --analyze verdict.          */
/*                                                                     */
/* Deliberately independent of the renderer so --analyze can use it    */
/* headless.                                                           */
/* ------------------------------------------------------------------ */

typedef struct SessionStats {
    uint64_t luma_bins[PROBE_HIST_BINS];
    uint64_t maxrgb_bins[PROBE_HIST_BINS];
    uint64_t black, underflow, valid;
    uint64_t maxrgb_valid;
    bool     has_maxrgb;

    /* CTA-861.3 quantities, measured. MaxCLL is the max over frames of
     * the per-frame max(R,G,B); MaxFALL the max over frames of the
     * per-frame average of max(R,G,B). Both are LOWER bounds when
     * sampling is strided or when only luma was available. */
    double   maxcll_nits;
    double   maxfall_nits;
    double   peak_luma_nits;
    double   min_frame_avg_nits;

    /* Spread decomposition, all in log2-nits (stops) over the `valid`
     * population, so the law of total variance actually applies:
     *     total^2 = spatial^2 + temporal^2
     * Weighted by per-frame sample count, which matters if resolution
     * changes mid-stream. */
    uint64_t frames;
    double   w_sum;          /* sum of n_f                             */
    double   w_mu_sum;       /* sum of n_f * mu_f                      */
    double   w_mu2_sum;      /* sum of n_f * mu_f^2                    */
    double   within_var_sum; /* sum of n_f * Var_within(f)             */

    /* Dedupe / coverage. A PTS high-water mark rather than a frame
     * index: playback is monotonic between seeks, so "pts > high_water"
     * is exact, needs no frame count or frame rate, works on VFR, and
     * cannot collide or double-count. It under-counts after a backward
     * seek (frames below the mark are skipped even if never seen), but
     * it never silently corrupts — the property a verdict needs. */
    bool     have_pts;
    int64_t  high_water_pts;
    int64_t  last_dur_pts;
    int64_t  start_pts;
    double   duration_sec;   /* <= 0 when unknown                      */
    double   tb_sec;         /* stream timebase in seconds             */
    double   covered_sec;

    /* Set when frame geometry or colour coding changes mid-stream. The
     * accumulator resets rather than pooling incomparable histograms;
     * this records that it happened. */
    bool     saw_format_change;
    int      width, height;
    int      pix_fmt;
    int      color_trc;
    int      color_range;

    LumReference reference;
    double       hlg_lw;
    bool         any_frames_unsupported;
} SessionStats;

/* `tb_sec` is the stream timebase (av_q2d of AVStream::time_base) and
 * `duration_sec` the stream duration, or <= 0 if unknown. */
void session_stats_init(SessionStats *s, double tb_sec, double duration_sec);
void session_stats_reset(SessionStats *s);

/* Feed one decoded frame. `pts` is the frame's presentation timestamp
 * in stream timebase units, or AV_NOPTS_VALUE. `dur` is the frame's
 * duration in the same units, or 0 if unknown.
 *
 * The duration matters for coverage: a PTS marks where a frame STARTS,
 * so the last frame of a file sits one interval short of the duration.
 * Without adding it, coverage tops out just under 100% and a complete
 * scan is permanently reported as partial. Returns true if the frame
 * was accumulated, false if skipped as already-seen. */
bool session_stats_add(SessionStats *s, int64_t pts, int64_t dur,
                       const FrameStats *f);

/* Note that a frame could not be measured (unsupported pixel format).
 * Kept distinct from "no frames" so callers can report the difference. */
void session_stats_note_unsupported(SessionStats *s);

/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t frames;
    double   coverage;        /* 0..1, or -1 when duration is unknown  */

    double   p1, p50, p99, p99_9;
    double   dr_stops;        /* log2(p99.9 / p1)                      */

    double   spatial_stops;   /* sqrt(mean within-frame variance)      */
    double   temporal_stops;  /* sqrt(variance of frame means)         */
    double   total_stops;     /* sqrt(spatial^2 + temporal^2)          */

    double   avg_nits;        /* exp2 of the pooled log2 mean          */
    double   maxcll_nits, maxfall_nits;
    bool     maxcll_valid;    /* false when only luma was measured     */

    double   black_pct, underflow_pct;
    double   peak_luma_nits;

    LumReference reference;
    double       hlg_lw;
} SessionDerived;

void session_stats_derive(const SessionStats *s, SessionDerived *out);

#endif
