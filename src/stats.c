#include "stats.h"

#include <math.h>
#include <string.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>

void session_stats_init(SessionStats *s, double tb_sec, double duration_sec)
{
    memset(s, 0, sizeof(*s));
    s->tb_sec        = tb_sec > 0.0 ? tb_sec : 0.0;
    s->duration_sec  = duration_sec > 0.0 ? duration_sec : 0.0;
    s->high_water_pts = INT64_MIN;
    s->min_frame_avg_nits = INFINITY;
    s->width = s->height = -1;
    s->pix_fmt = s->color_trc = s->color_range = -1;
}

void session_stats_reset(SessionStats *s)
{
    double tb  = s->tb_sec;
    double dur = s->duration_sec;
    session_stats_init(s, tb, dur);
}

void session_stats_note_unsupported(SessionStats *s)
{
    s->any_frames_unsupported = true;
}

/* Frame geometry / colour coding must match for histograms to be
 * poolable. A mid-stream change would otherwise silently mix
 * incomparable distributions with unequal weighting, so start over. */
static void adopt_format(SessionStats *s, const FrameStats *f)
{
    s->width       = f->width;
    s->height      = f->height;
    s->pix_fmt     = f->pix_fmt;
    s->color_trc   = f->color_trc;
    s->color_range = f->color_range;
    s->reference   = f->reference;
    s->hlg_lw      = f->hlg_lw;
}

static bool format_changed(const SessionStats *s, const FrameStats *f)
{
    return s->width       != f->width     || s->height      != f->height ||
           s->pix_fmt     != f->pix_fmt   || s->color_trc   != f->color_trc ||
           s->color_range != f->color_range;
}

bool session_stats_add(SessionStats *s, int64_t pts, int64_t dur,
                       const FrameStats *f)
{
    if (!s || !f) return false;

    /* Dedupe. Anything at or below the high-water mark has already been
     * folded in — that covers both --loop rewinds and backward seeks. */
    if (pts != AV_NOPTS_VALUE) {
        if (s->have_pts && pts <= s->high_water_pts) return false;
        if (!s->have_pts) { s->start_pts = pts; s->have_pts = true; }
        s->high_water_pts = pts;
        if (dur > 0) s->last_dur_pts = dur;
        /* A PTS marks where a frame STARTS, so the span actually seen
         * runs to the END of the newest frame. Omitting that trailing
         * interval leaves a complete scan reporting 99%. */
        if (s->tb_sec > 0.0)
            s->covered_sec = (double)(s->high_water_pts - s->start_pts +
                                      s->last_dur_pts) * s->tb_sec;
    }

    if (s->frames == 0) {
        adopt_format(s, f);
    } else if (format_changed(s, f)) {
        /* Coding changed under us; the histograms are no longer
         * comparable. Preserve the timing state so coverage does not
         * restart, but drop the distributions. */
        bool    had_pts = s->have_pts;
        int64_t hw = s->high_water_pts, st = s->start_pts;
        double  cov = s->covered_sec;
        session_stats_reset(s);
        s->have_pts       = had_pts;
        s->high_water_pts = hw;
        s->start_pts      = st;
        s->covered_sec    = cov;
        s->saw_format_change = true;
        adopt_format(s, f);
    }

    for (int i = 0; i < PROBE_HIST_BINS; i++)
        s->luma_bins[i] += f->luma.bins[i];
    s->black     += f->luma.black;
    s->underflow += f->luma.underflow;
    s->valid     += f->luma.valid;

    if (f->has_maxrgb) {
        for (int i = 0; i < PROBE_HIST_BINS; i++)
            s->maxrgb_bins[i] += f->maxrgb.bins[i];
        s->maxrgb_valid += f->maxrgb.valid;
        s->has_maxrgb = true;
        if (f->maxrgb_peak > s->maxcll_nits)  s->maxcll_nits  = f->maxrgb_peak;
        if (f->maxrgb_avg  > s->maxfall_nits) s->maxfall_nits = f->maxrgb_avg;
    } else {
        /* Luma-only: still track the best available bounds so the live
         * panel has something to show, but has_maxrgb stays false so
         * callers know these are not MaxCLL/MaxFALL to spec. */
        if (f->peak_nits > s->maxcll_nits)  s->maxcll_nits  = f->peak_nits;
        if (f->avg_nits  > s->maxfall_nits) s->maxfall_nits = f->avg_nits;
    }

    if (f->peak_nits > s->peak_luma_nits) s->peak_luma_nits = f->peak_nits;
    if (f->avg_nits < s->min_frame_avg_nits) s->min_frame_avg_nits = f->avg_nits;

    /* Spread accumulators, weighted by the frame's valid sample count.
     * mu_log2/var_log2 are NAN when a frame had no valid samples (an
     * all-black frame, say) — those contribute nothing rather than
     * poisoning the sums. */
    if (f->luma.valid > 0 && isfinite(f->mu_log2) && isfinite(f->var_log2)) {
        double n = (double)f->luma.valid;
        s->w_sum          += n;
        s->w_mu_sum       += n * f->mu_log2;
        s->w_mu2_sum      += n * f->mu_log2 * f->mu_log2;
        s->within_var_sum += n * f->var_log2;
    }

    s->frames++;
    return true;
}

/* ------------------------------------------------------------------ */
static double hist_percentile_u64(const uint64_t *bins, uint64_t total, double pct)
{
    if (!total) return NAN;
    double target = pct / 100.0 * (double)total;
    uint64_t acc = 0;
    for (int i = 0; i < PROBE_HIST_BINS; i++) {
        acc += bins[i];
        if ((double)acc >= target && bins[i]) return probe_bin_to_nits(i);
    }
    for (int i = PROBE_HIST_BINS - 1; i >= 0; i--)
        if (bins[i]) return probe_bin_to_nits(i);
    return NAN;
}

void session_stats_derive(const SessionStats *s, SessionDerived *out)
{
    memset(out, 0, sizeof(*out));
    out->coverage     = -1.0;
    out->reference    = s->reference;
    out->hlg_lw       = s->hlg_lw;
    out->frames       = s->frames;
    out->maxcll_nits  = s->maxcll_nits;
    out->maxfall_nits = s->maxfall_nits;
    out->maxcll_valid = s->has_maxrgb;
    out->peak_luma_nits = s->peak_luma_nits;

    /* Coding limits, so a dynamic-range figure can be read against what
     * the format could actually carry rather than in isolation. */
    if (s->frames) {
        const AVPixFmtDescriptor *pd = (s->pix_fmt >= 0)
            ? av_pix_fmt_desc_get((enum AVPixelFormat)s->pix_fmt) : NULL;
        out->bit_depth  = pd ? pd->comp[0].depth : 8;
        out->full_range = (s->color_range == AVCOL_RANGE_JPEG);
        out->dr_ceiling_stops = probe_dr_ceiling_stops(
            (enum AVColorTransferCharacteristic)s->color_trc,
            out->bit_depth, out->full_range, s->hlg_lw);
    }

    if (s->duration_sec > 0.0 && s->covered_sec >= 0.0) {
        double c = s->covered_sec / s->duration_sec;
        out->coverage = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
    }

    uint64_t pop = s->black + s->underflow + s->valid;
    if (pop) {
        out->black_pct     = 100.0 * (double)s->black     / (double)pop;
        out->underflow_pct = 100.0 * (double)s->underflow / (double)pop;
    }

    if (!s->valid) {
        out->p1 = out->p50 = out->p99 = out->p99_9 = NAN;
        out->avg_nits = NAN;
        return;
    }

    out->p1    = hist_percentile_u64(s->luma_bins, s->valid, 1.0);
    out->p50   = hist_percentile_u64(s->luma_bins, s->valid, 50.0);
    out->p99   = hist_percentile_u64(s->luma_bins, s->valid, 99.0);
    out->p99_9 = hist_percentile_u64(s->luma_bins, s->valid, 99.9);
    out->dr_stops = (isfinite(out->p1) && isfinite(out->p99_9) &&
                     out->p1 > 0.0 && out->p99_9 > 0.0)
                    ? log2(out->p99_9 / out->p1) : 0.0;

    /* Law of total variance. Every term is log2-domain over the same
     * `valid` population, which is what makes the identity hold:
     *     Var_total = E[Var_within] + Var[E_within]
     * The self-consistency of total vs sqrt(spatial^2 + temporal^2) is
     * asserted in the tests. */
    if (s->w_sum > 0.0) {
        double mean = s->w_mu_sum / s->w_sum;
        double temporal_var = s->w_mu2_sum / s->w_sum - mean * mean;
        if (temporal_var < 0.0) temporal_var = 0.0;   /* rounding */
        double spatial_var = s->within_var_sum / s->w_sum;
        if (spatial_var < 0.0) spatial_var = 0.0;

        out->temporal_stops = sqrt(temporal_var);
        out->spatial_stops  = sqrt(spatial_var);
        out->total_stops    = sqrt(spatial_var + temporal_var);
        out->avg_nits       = exp2(mean);
    } else {
        out->avg_nits = NAN;
    }
}
