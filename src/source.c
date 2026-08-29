#include "source.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/frame.h>

/* ------------------------------------------------------------------ */
/* Frame ring                                                          */
/* ------------------------------------------------------------------ */
void ring_init(FrameRing *r, int cap)
{
    memset(r, 0, sizeof(*r));
    if (cap <= 0) return;
    r->buf = calloc(cap, sizeof(*r->buf));
    r->cap = r->buf ? cap : 0;
}

void ring_clear(FrameRing *r)
{
    for (int i = 0; i < r->len; i++) av_frame_free(&r->buf[i]);
    r->len = 0;
    r->back = 0;
}

void ring_free(FrameRing *r)
{
    ring_clear(r);
    free(r->buf);
    r->buf = NULL;
    r->cap = 0;
}

void ring_push(FrameRing *r, AVFrame *f)
{
    if (r->cap <= 0) { av_frame_free(&f); return; }

    /* Pushing while the cursor is parked in the past would leave the
     * history branching. Playback always resumes live, so discard the
     * frames ahead of the cursor first. */
    if (r->back > 0) {
        for (int i = r->len - r->back; i < r->len; i++) av_frame_free(&r->buf[i]);
        r->len -= r->back;
        r->back = 0;
    }

    if (r->len == r->cap) {
        av_frame_free(&r->buf[0]);
        memmove(&r->buf[0], &r->buf[1], (size_t)(r->cap - 1) * sizeof(*r->buf));
        r->len--;
    }
    r->buf[r->len++] = f;
}

AVFrame *ring_current(const FrameRing *r)
{
    int idx = r->len - 1 - r->back;
    if (idx < 0 || idx >= r->len) return NULL;
    return r->buf[idx];
}

bool ring_step_back(FrameRing *r)
{
    if (r->back + 1 >= r->len) return false;
    r->back++;
    return true;
}

bool ring_step_fwd(FrameRing *r)
{
    if (r->back <= 0) return false;
    r->back--;
    return true;
}

/* ------------------------------------------------------------------ */
/* Source                                                              */
/* ------------------------------------------------------------------ */
static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static double pts_to_sec(const Source *s, const AVFrame *f)
{
    if (!f || f->best_effort_timestamp == AV_NOPTS_VALUE) return NAN;
    return (double)f->best_effort_timestamp * s->tb_sec;
}

bool source_open(Source *s, const char *path, int ring_cap)
{
    memset(s, 0, sizeof(*s));
    if (!decoder_open(&s->dec, path)) return false;

    AVStream *st = s->dec.fmt->streams[s->dec.stream_idx];
    s->tb_sec = av_q2d(st->time_base);
    if (st->duration > 0 && st->duration != AV_NOPTS_VALUE)
        s->duration_sec = (double)st->duration * s->tb_sec;
    else if (s->dec.fmt->duration > 0 && s->dec.fmt->duration != AV_NOPTS_VALUE)
        s->duration_sec = (double)s->dec.fmt->duration / AV_TIME_BASE;

    snprintf(s->label, sizeof(s->label), "%s", basename_of(path));
    ring_init(&s->ring, ring_cap);
    session_stats_init(&s->session, s->tb_sec, s->duration_sec);
    s->frame_no = -1;
    return true;
}

void source_close(Source *s)
{
    av_frame_free(&s->shown);
    av_frame_free(&s->pending);
    ring_free(&s->ring);
    decoder_close(&s->dec);
}

double source_shown_sec(const Source *s)
{
    return pts_to_sec(s, s->shown);
}

void source_flush(Source *s)
{
    av_frame_free(&s->pending);
    ring_clear(&s->ring);
    s->eof = false;
}

/* Pull one frame into `pending` if there isn't one already. */
static bool fill_pending(Source *s)
{
    if (s->pending) return true;
    if (s->eof) return false;

    int r = decoder_next_frame(&s->dec);
    if (r <= 0) { s->eof = true; return false; }

    decoder_absorb_frame_side_data(&s->dec);
    s->pending = av_frame_clone(s->dec.frame);
    return s->pending != NULL;
}

double source_peek_next_sec(Source *s)
{
    if (!fill_pending(s)) return NAN;
    return pts_to_sec(s, s->pending);
}

/* Promote `pending` to `shown`, folding it into the ring and stats. */
static void promote(Source *s)
{
    AVFrame *f = s->pending;
    s->pending = NULL;

    s->frame_stats_valid = probe_frame_stats(f, 8, PROBE_LUMA_ONLY,
                                             &s->frame_stats);
    if (s->frame_stats_valid)
        session_stats_add(&s->session, f->best_effort_timestamp,
                          f->duration, &s->frame_stats);
    else
        session_stats_note_unsupported(&s->session);

    s->frame_no++;

    if (s->ring.cap > 0) {
        ring_push(&s->ring, av_frame_clone(f));
        av_frame_free(&s->shown);
        s->shown = f;
    } else {
        av_frame_free(&s->shown);
        s->shown = f;
    }
}

bool source_advance_to(Source *s, double t)
{
    bool changed = false;

    /* Live playback always resumes at the newest frame. */
    while (s->ring.back > 0) { ring_step_fwd(&s->ring); changed = true; }
    if (changed) {
        AVFrame *cur = ring_current(&s->ring);
        if (cur) { av_frame_free(&s->shown); s->shown = av_frame_clone(cur); }
    }

    for (;;) {
        double next = source_peek_next_sec(s);
        /* No PTS: fall back to "one frame per call" so the stream still
         * plays rather than stalling forever on an un-comparable time. */
        if (isnan(next)) {
            if (!s->pending) break;
            promote(s);
            changed = true;
            break;
        }
        if (next > t) break;
        promote(s);
        changed = true;
    }
    return changed;
}

double source_step_forward(Source *s)
{
    /* Walk back toward live before decoding anything new. */
    if (ring_step_fwd(&s->ring)) {
        AVFrame *cur = ring_current(&s->ring);
        if (cur) {
            av_frame_free(&s->shown);
            s->shown = av_frame_clone(cur);
            return pts_to_sec(s, s->shown);
        }
    }
    if (!fill_pending(s)) return NAN;
    promote(s);
    return pts_to_sec(s, s->shown);
}

double source_step_back(Source *s)
{
    if (ring_step_back(&s->ring)) {
        AVFrame *cur = ring_current(&s->ring);
        if (cur) {
            av_frame_free(&s->shown);
            s->shown = av_frame_clone(cur);
            return pts_to_sec(s, s->shown);
        }
    }

    /* Ring exhausted (or disabled): seek behind the current frame and
     * decode forward to the frame immediately preceding it. The window
     * is a guess at GOP length; if the seek lands too late we widen it
     * rather than returning the same frame and appearing stuck. */
    double cur_t = source_shown_sec(s);
    if (isnan(cur_t)) return NAN;

    for (double window = 0.5; window <= 8.0; window *= 4.0) {
        double target = cur_t - window;
        if (target < 0.0) target = 0.0;
        if (!decoder_seek_to(&s->dec, target)) return NAN;
        source_flush(s);

        /* Decode forward, keeping the last frame strictly before cur_t.
         * Everything on the way refills the ring, so the next few
         * back-steps are instant again. */
        double best = NAN;
        for (;;) {
            double next = source_peek_next_sec(s);
            if (isnan(next) || next >= cur_t - 1e-9) break;
            promote(s);
            best = pts_to_sec(s, s->shown);
        }
        if (!isnan(best)) return best;
        if (target <= 0.0) break;   /* already at the start */
    }
    return NAN;
}
