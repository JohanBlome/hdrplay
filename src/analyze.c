#include "analyze.h"
#include "checks.h"
#include "decoder.h"
#include "log.h"
#include "probe.h"
#include "stats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/mastering_display_metadata.h>

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

/* MaxCLL/MaxFALL measurement is biased LOW by three independent
 * effects, and the direction is what makes the check usable:
 *
 *   1. sampling stride skips the specular glints that set MaxCLL
 *   2. luma-only mode measures Y', not the spec's max(R,G,B)
 *   3. the YUV matrix is applied to NONLINEAR R'G'B' and every EOTF
 *      here is convex, so by Jensen eotf(Y') <= true luminance
 *
 * So the measurement is a one-sided lower bound. `measured > declared`
 * is a contradiction and therefore real evidence of under-declaration.
 * `measured <= declared` proves nothing at all — reporting that as PASS
 * would green-light exactly the broken files this tool exists to catch,
 * so it is reported as INFO. */
static void check_declared(const char *label, double measured, int declared,
                           bool exact)
{
    char buf[160];
    if (declared <= 0) {
        snprintf(buf, sizeof(buf), ">= %.0fN measured, container declares none",
                 measured);
        check(R_INFO, label, buf);
        return;
    }
    if (measured > declared) {
        snprintf(buf, sizeof(buf),
                 "declares %dN but pixels reach %.0fN%s",
                 declared, measured, exact ? "" : " (lower bound)");
        check(R_FAIL, label, buf);
        snprintf(buf, sizeof(buf),
                 "under-declared: tone mappers trust this value");
        check_note(buf);
    } else {
        snprintf(buf, sizeof(buf),
                 "declares %dN, measured >= %.0fN — not disproven",
                 declared, measured);
        check(R_INFO, label, buf);
    }
}

/* Dynamic range, reported against what the coding could carry.
 *
 * log2(p99.9/p1) is only meaningful when the transfer has a floor. PQ
 * has one by definition. SDR's comes from the BT.1886 black level, and
 * with that set to zero the figure diverges toward the quantization
 * limit — 18.7 stops on 8-bit limited range — no matter what the
 * picture contains. Printing the ceiling next to the measurement is
 * what makes the number readable: "9.4 of a possible 9.7" says the
 * file uses its format; a bare "9.4 stops" says nothing.
 *
 * Exceeding the ceiling is not a content finding but a measurement
 * one: p1 has landed in the code lattice. */
static void check_dynamic_range(const SessionDerived *d)
{
    char buf[192];
    const char *rng = d->full_range ? "full" : "limited";

    if (isfinite(d->dr_ceiling_stops) && d->dr_ceiling_stops > 0.0)
        snprintf(buf, sizeof(buf), "%.1f stops (p99.9/p1) of %.1f possible "
                 "at %d-bit %s range",
                 d->dr_stops, d->dr_ceiling_stops, d->bit_depth, rng);
    else
        snprintf(buf, sizeof(buf), "%.1f stops (p99.9/p1)", d->dr_stops);

    /* The 0.25 slack is the histogram's own resolution: 1/16-stop bins
     * put ±0.06 on each percentile, and the ceiling is computed in
     * closed form rather than through the bins. */
    bool over = isfinite(d->dr_ceiling_stops) &&
                d->dr_stops > d->dr_ceiling_stops + 0.25;

    check(over ? R_WARN : (d->dr_stops < 6.0 ? R_WARN : R_PASS),
          "dynamic range", buf);

    if (over)
        check_note("above what the coding can express — p1 is in the "
                   "quantization floor, not the picture");
    if (d->reference == LUM_SDR_RELATIVE && !(probe_sdr_black_nits() > 0.0))
        check_note("--sdr-black 0: no BT.1886 floor, so this ratio "
                   "describes the code lattice rather than a displayable range");
}

void analyze_print_session(const SessionStats *s, const Decoder *dec,
                           const char *title)
{
    int declared_cll_max = (dec && dec->has_cll) ? dec->cll_max : -1;
    int declared_cll_avg = (dec && dec->has_cll) ? dec->cll_avg : -1;

    SessionDerived d;
    session_stats_derive(s, &d);

    fprintf(stderr, "\n%s\n", title);

    if (d.frames == 0) {
        check(R_WARN, "frames measured",
              s->any_frames_unsupported
                ? "none — pixel format not readable by the probe"
                : "none");
        check_summary();
        return;
    }

    char buf[192];

    /* Coverage first: it gates how much the rest is worth. */
    if (d.coverage >= 0.0) {
        snprintf(buf, sizeof(buf), "%llu frames, %.0f%% of duration",
                 (unsigned long long)d.frames, d.coverage * 100.0);
        check(d.coverage >= 0.999 ? R_PASS : R_WARN, "coverage", buf);
        if (d.coverage < 0.999)
            check_note("partial scan — absolute figures are lower bounds only");
    } else {
        snprintf(buf, sizeof(buf), "%llu frames, duration unknown",
                 (unsigned long long)d.frames);
        check(R_INFO, "coverage", buf);
    }

    if (s->any_frames_unsupported)
        check(R_WARN, "unreadable frames",
              "some frames had a pixel format the probe cannot read");
    if (s->saw_format_change)
        check(R_WARN, "format change",
              "coding changed mid-stream; statistics restarted");

    /* Before any luminance figure, because it is upstream of all of
     * them: reading a full-range source as limited crushes codes 1..15
     * into "black" and divides everything just above by roughly ten. */
    if (dec && dec->range_guessed) {
        snprintf(buf, sizeof(buf),
                 "container declares none — %.3f%% of luma outside the "
                 "limited-range window, read as %s",
                 dec->range_outside_frac * 100.0,
                 d.full_range ? "full" : "limited");
        check(R_INFO, "colour range recovered", buf);
        check_note("override with --range limited|full if this is wrong");
    }

    snprintf(buf, sizeof(buf), "%s", lum_reference_name(d.reference));
    check(R_INFO, "luminance reference", buf);
    if (d.reference == LUM_SDR_RELATIVE) {
        double lb = probe_sdr_black_nits();
        if (lb > 0.0)
            snprintf(buf, sizeof(buf),
                     "BT.1886, %.3g-nit black (--sdr-black)", lb);
        else
            snprintf(buf, sizeof(buf),
                     "bare 100·V^2.4, no black floor (--sdr-black 0)");
        check_note(buf);
    }
    if (d.reference == LUM_HLG_OOTF) {
        /* Name the source, not just the number. HLG carries no absolute
         * luminance, so every nit below rests on this assumption, and
         * "1000 because the file said so" and "1000 because nothing did"
         * are very different claims. */
        const char *src = (probe_hlg_peak_override_nits() > 0.0) ? "--hlg-peak"
                        : (dec && dec->has_mastering_display)    ? "mastering display"
                                                                 : "BT.2100 default";
        snprintf(buf, sizeof(buf),
                 "all nits below assume a %.0fN display peak (%s)", d.hlg_lw, src);
        check_note(buf);
    }

    bool absolute = lum_reference_is_absolute(d.reference);

    /* Mislabelled SDR in an HDR container. Absolute references only —
     * for an SDR source a sub-100-nit peak is the definition, not a
     * defect. */
    if (absolute) {
        if (d.p99_9 < 100.0) {
            snprintf(buf, sizeof(buf),
                     "p99.9 is only %.0fN — never exceeds SDR range", d.p99_9);
            check(R_FAIL, "HDR container, SDR content", buf);
        } else {
            snprintf(buf, sizeof(buf), "p99.9 = %.0fN", d.p99_9);
            check(R_PASS, "content exceeds SDR range", buf);
        }

        check_declared("MaxCLL vs declared",  d.maxcll_nits,
                       declared_cll_max, d.maxcll_valid);
        check_declared("MaxFALL vs declared", d.maxfall_nits,
                       declared_cll_avg, d.maxcll_valid);
        if (!d.maxcll_valid)
            check_note("luma-only scan: MaxCLL is defined over max(R,G,B)");
    } else {
        check(R_INFO, "absolute checks",
              "suppressed — SDR source has no absolute luminance");
    }

    check_dynamic_range(&d);

    snprintf(buf, sizeof(buf), "p1 %.3fN  p50 %.1fN  p99 %.0fN  p99.9 %.0fN",
             d.p1, d.p50, d.p99, d.p99_9);
    check(R_INFO, "luminance distribution", buf);

    snprintf(buf, sizeof(buf), "%.2f spatial / %.2f temporal stops",
             d.spatial_stops, d.temporal_stops);
    check(R_INFO, "spread", buf);

    snprintf(buf, sizeof(buf), "%.1f%% black, %.2f%% below histogram floor",
             d.black_pct, d.underflow_pct);
    check(R_INFO, "shadow population", buf);

    check_summary();
}

/* ------------------------------------------------------------------ */
/* NDJSON per-frame stats file                                         */
/* ------------------------------------------------------------------ */
/* Streams straight out of the decode loop with no buffering. Per-frame
 * records are scalars only: 512 histogram bins times 170k frames is not
 * a file anyone wants. Session histograms go in the trailer, once. */
static void stats_write_header(FILE *fp, const Decoder *d, int stride)
{
    fprintf(fp,
        "{\"type\":\"header\",\"schema\":1,\"width\":%d,\"height\":%d,"
        "\"bit_depth\":%d,\"stride\":%d,\"transfer\":%d,\"primaries\":%d,"
        "\"declared_maxcll\":%d,\"declared_maxfall\":%d}\n",
        d->width, d->height, d->bit_depth, stride,
        (int)d->transfer, (int)d->primaries,
        d->has_cll ? d->cll_max : -1,
        d->has_cll ? d->cll_avg : -1);
}

static void stats_write_frame(FILE *fp, int64_t idx, double pts_sec,
                              const FrameStats *f)
{
    fprintf(fp,
        "{\"type\":\"frame\",\"i\":%lld,\"t\":%.6f,"
        "\"luma_peak\":%.4f,\"luma_avg\":%.4f,"
        "\"maxrgb_peak\":%.4f,\"maxrgb_avg\":%.4f,"
        "\"mu_log2\":%.6f,\"var_log2\":%.6f,"
        "\"black\":%llu,\"underflow\":%llu,\"valid\":%llu,"
        "\"out709_pct\":%.4f}\n",
        (long long)idx, pts_sec,
        f->peak_nits, f->avg_nits,
        f->has_maxrgb ? f->maxrgb_peak : -1.0,
        f->has_maxrgb ? f->maxrgb_avg  : -1.0,
        f->mu_log2, f->var_log2,
        (unsigned long long)f->luma.black,
        (unsigned long long)f->luma.underflow,
        (unsigned long long)f->luma.valid,
        f->pct_outside_709);
}

/* The header's declared_* fields are stream-level and written before
 * decoding, so they miss files that carry CLL only as per-frame side
 * data. Repeat the final values here, where they are settled. */
static void stats_write_trailer(FILE *fp, const SessionStats *s,
                                int declared_max, int declared_avg)
{
    SessionDerived d;
    session_stats_derive(s, &d);
    fprintf(fp, "{\"type\":\"declared\",\"maxcll\":%d,\"maxfall\":%d}\n",
            declared_max, declared_avg);
    fprintf(fp,
        "{\"type\":\"summary\",\"frames\":%llu,\"coverage\":%.6f,"
        "\"reference\":\"%s\",\"hlg_lw\":%.1f,"
        "\"maxcll\":%.4f,\"maxfall\":%.4f,\"maxcll_exact\":%s,"
        "\"p1\":%.6f,\"p50\":%.4f,\"p99\":%.4f,\"p99_9\":%.4f,"
        "\"dr_stops\":%.4f,\"dr_ceiling_stops\":%.4f,"
        "\"sdr_black_nits\":%.4f,\"bit_depth\":%d,\"full_range\":%s,"
        "\"spatial_stops\":%.4f,\"temporal_stops\":%.4f,"
        "\"total_stops\":%.4f,\"black_pct\":%.4f,\"underflow_pct\":%.4f}\n",
        (unsigned long long)d.frames, d.coverage,
        lum_reference_name(d.reference), d.hlg_lw,
        d.maxcll_nits, d.maxfall_nits, d.maxcll_valid ? "true" : "false",
        d.p1, d.p50, d.p99, d.p99_9,
        d.dr_stops, d.dr_ceiling_stops,
        probe_sdr_black_nits(), d.bit_depth, d.full_range ? "true" : "false",
        d.spatial_stops, d.temporal_stops, d.total_stops,
        d.black_pct, d.underflow_pct);

    fputs("{\"type\":\"histogram\",\"which\":\"luma\",\"bins\":[", fp);
    for (int i = 0; i < PROBE_HIST_BINS; i++)
        fprintf(fp, "%s%llu", i ? "," : "", (unsigned long long)s->luma_bins[i]);
    fprintf(fp, "],\"min_log2\":%.1f,\"per_stop\":%.1f}\n",
            PROBE_HIST_MIN_LOG2, PROBE_HIST_PER_STOP);

    if (s->has_maxrgb) {
        fputs("{\"type\":\"histogram\",\"which\":\"maxrgb\",\"bins\":[", fp);
        for (int i = 0; i < PROBE_HIST_BINS; i++)
            fprintf(fp, "%s%llu", i ? "," : "",
                    (unsigned long long)s->maxrgb_bins[i]);
        fprintf(fp, "],\"min_log2\":%.1f,\"per_stop\":%.1f}\n",
                PROBE_HIST_MIN_LOG2, PROBE_HIST_PER_STOP);
    }
}

/* --json goes to stdout so `| jq` works; the human report is on stderr. */
static void emit_json(const SessionStats *s, const Decoder *d, const char *path)
{
    SessionDerived v;
    session_stats_derive(s, &v);
    printf("{\"file\":\"%s\",\"frames\":%llu,\"coverage\":%.6f,"
           "\"reference\":\"%s\",\"hlg_lw\":%.1f,"
           "\"declared_maxcll\":%d,\"declared_maxfall\":%d,"
           "\"measured_maxcll\":%.4f,\"measured_maxfall\":%.4f,"
           "\"maxcll_exact\":%s,"
           "\"bit_depth\":%d,\"full_range\":%s,\"range_guessed\":%s,"
           "\"range_outside_frac\":%.6f,\"sdr_black_nits\":%.4f,"
           "\"p1\":%.6f,\"p50\":%.4f,\"p99\":%.4f,\"p99_9\":%.4f,"
           "\"dr_stops\":%.4f,\"dr_ceiling_stops\":%.4f,"
           "\"spatial_stops\":%.4f,"
           "\"temporal_stops\":%.4f,\"total_stops\":%.4f,"
           "\"black_pct\":%.4f,\"underflow_pct\":%.4f,"
           "\"fails\":%d,\"warns\":%d}\n",
           path, (unsigned long long)v.frames, v.coverage,
           lum_reference_name(v.reference), v.hlg_lw,
           d->has_cll ? d->cll_max : -1, d->has_cll ? d->cll_avg : -1,
           v.maxcll_nits, v.maxfall_nits, v.maxcll_valid ? "true" : "false",
           v.bit_depth, v.full_range ? "true" : "false",
           d->range_guessed ? "true" : "false", d->range_outside_frac,
           probe_sdr_black_nits(),
           v.p1, v.p50, v.p99, v.p99_9,
           v.dr_stops, v.dr_ceiling_stops,
           v.spatial_stops, v.temporal_stops, v.total_stops,
           v.black_pct, v.underflow_pct,
           check_fail_count(), check_warn_count());
}

/* ------------------------------------------------------------------ */
int analyze_run(const char *path, int stride, double hlg_lw,
                bool json, const char *stats_path)
{
    /* <= 0 leaves the per-file resolution in place (mastering display,
     * else the BT.2100 1000-nit reference). */
    probe_set_hlg_peak_override(hlg_lw);

    check_reset();

    Decoder dec;
    if (!decoder_open(&dec, path)) {
        fprintf(stderr, "analyze: cannot open %s\n", path);
        return HDRPLAY_EXIT_TOOL_ERROR;
    }
    /* Must settle before the first frame is measured: the range decides
     * where black is, and the histogram cannot be re-binned afterwards. */
    decoder_resolve_color_range(&dec, 8);

    AVStream *vs = dec.fmt->streams[dec.stream_idx];
    AVRational tb = vs->time_base;
    double duration_sec = 0.0;
    if (vs->duration > 0 && vs->duration != AV_NOPTS_VALUE)
        duration_sec = (double)vs->duration * av_q2d(tb);
    else if (dec.fmt->duration > 0 && dec.fmt->duration != AV_NOPTS_VALUE)
        duration_sec = (double)dec.fmt->duration / AV_TIME_BASE;

    SessionStats s;
    session_stats_init(&s, av_q2d(tb), duration_sec);

    FILE *sf = NULL;
    if (stats_path) {
        sf = fopen(stats_path, "w");
        if (!sf) {
            fprintf(stderr, "analyze: cannot write %s\n", stats_path);
            decoder_close(&dec);
            return HDRPLAY_EXIT_TOOL_ERROR;
        }
        stats_write_header(sf, &dec, stride);
    }

    int64_t idx = 0;
    int     rc;
    bool    decode_error = false;
    FrameStats fs;

    while ((rc = decoder_next_frame(&dec)) > 0) {
        /* Files that carry CLL only per frame would otherwise get no
         * comparison at all. */
        decoder_absorb_frame_side_data(&dec);

        if (probe_frame_stats(dec.frame, stride, PROBE_FULL_RGB, &fs)) {
            session_stats_add(&s, dec.frame->best_effort_timestamp,
                              dec.frame->duration, &fs);
            if (sf) {
                double t = (dec.frame->best_effort_timestamp == AV_NOPTS_VALUE)
                    ? NAN
                    : (double)dec.frame->best_effort_timestamp * av_q2d(tb);
                stats_write_frame(sf, idx, t, &fs);
            }
        } else if (probe_frame_stats(dec.frame, stride, PROBE_LUMA_ONLY, &fs)) {
            /* Chroma unreadable (NV12 and friends) but luma is fine.
             * Degrade to luma-only rather than measuring nothing —
             * has_maxrgb stays false so the report says MaxCLL is not
             * to spec. */
            session_stats_add(&s, dec.frame->best_effort_timestamp,
                              dec.frame->duration, &fs);
            if (sf) {
                double t = (dec.frame->best_effort_timestamp == AV_NOPTS_VALUE)
                    ? NAN
                    : (double)dec.frame->best_effort_timestamp * av_q2d(tb);
                stats_write_frame(sf, idx, t, &fs);
            }
        } else {
            session_stats_note_unsupported(&s);
        }
        idx++;
    }
    if (rc < 0) decode_error = true;

    if (sf) {
        stats_write_trailer(sf, &s,
                            dec.has_cll ? dec.cll_max : -1,
                            dec.has_cll ? dec.cll_avg : -1);
        fclose(sf);
    }

    /* A decode abort mid-file means coverage is incomplete regardless
     * of what the PTS high-water mark says — "walked to EOF" is not the
     * same as "walked all of it". */
    if (decode_error)
        check(R_WARN, "decode", "aborted before EOF — coverage incomplete");

    char title[512];
    snprintf(title, sizeof(title), "content checks  %s", path);
    analyze_print_session(&s, &dec, title);

    if (json) emit_json(&s, &dec, path);

    int fails = check_fail_count();
    bool measured_nothing = (s.frames == 0);
    decoder_close(&dec);

    if (measured_nothing) return HDRPLAY_EXIT_TOOL_ERROR;
    return fails;
}
