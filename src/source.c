#include "source.h"
#include "log.h"

#include <math.h>
#include <limits.h>
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

/* The HUD counter describes the frame currently displayed, not how many
 * frames this decoder instance has processed. A decode counter cannot move
 * backward and becomes meaningless after a seek. Deriving it from PTS makes
 * ring navigation, seeks and separate runs of the same file agree. */
static int displayed_frame_no(const Source *s, const AVFrame *f, int fallback)
{
    double t = pts_to_sec(s, f);
    if (isnan(t) || !(s->fps > 0.0)) return fallback;
    double n = (t - s->start_sec) * s->fps;
    if (n < 0.0) n = 0.0;
    if (n > INT_MAX) return INT_MAX;
    return (int)llround(n);
}

bool source_open(Source *s, const char *path, int ring_cap)
{
    memset(s, 0, sizeof(*s));
    if (!decoder_open(&s->dec, path)) return false;
    /* Before the first frame reaches the renderer or the probe, so both
     * see the same range for an untagged source. */
    decoder_resolve_color_range(&s->dec, 8);

    AVStream *st = s->dec.fmt->streams[s->dec.stream_idx];
    s->tb_sec = av_q2d(st->time_base);
    AVRational rate = av_guess_frame_rate(s->dec.fmt, st, NULL);
    s->fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
    s->start_sec = st->start_time != AV_NOPTS_VALUE
                 ? (double)st->start_time * s->tb_sec : 0.0;
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
    av_frame_free(&s->previous);
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
    av_frame_free(&s->shown);
    av_frame_free(&s->previous);
    av_frame_free(&s->pending);
    ring_clear(&s->ring);
    s->eof = false;
    s->still_image = false;
    s->frames_presented = 0;
    s->frame_stats_valid = false;
}

/* Select a retained frame and, when temporal differencing is enabled,
 * select its immediate predecessor as well. Keeping this in one helper is
 * important for backward stepping: `shown`'s previous value is then the
 * frame we just stepped FROM, not the frame that precedes it. */
static void show_ring_current(Source *s)
{
    int idx = s->ring.len - 1 - s->ring.back;
    AVFrame *cur = ring_current(&s->ring);
    AVFrame *prev = idx > 0 ? s->ring.buf[idx - 1] : NULL;

    av_frame_free(&s->shown);
    av_frame_free(&s->previous);
    s->shown = cur ? av_frame_clone(cur) : NULL;
    if (s->shown)
        s->frame_no = displayed_frame_no(s, s->shown, s->frame_no);
    if (s->keep_previous && prev)
        s->previous = av_frame_clone(prev);
}

void source_keep_previous(Source *s, bool enable)
{
    s->keep_previous = enable;
    av_frame_free(&s->previous);
    if (!enable) return;

    int idx = s->ring.len - 1 - s->ring.back;
    if (idx > 0)
        s->previous = av_frame_clone(s->ring.buf[idx - 1]);
}

/* Pull one frame into `pending` if there isn't one already. */
static bool fill_pending(Source *s)
{
    if (s->pending) return true;
    if (s->eof) return false;

    int r = decoder_next_frame(&s->dec);
    if (r <= 0) {
        s->eof = true;
        /* A still image is exposed by FFmpeg as a one-frame video stream.
         * Detect it from behavior rather than filename extensions so every
         * decoder-backed image format receives the same treatment. */
        if (r == 0 && s->frames_presented == 1)
            s->still_image = true;
        return false;
    }

    decoder_absorb_frame_side_data(&s->dec);
    s->pending = av_frame_clone(s->dec.frame);
    return s->pending != NULL;
}

double source_peek_next_sec(Source *s)
{
    /* When parked in retained history, the next frame is already in the
     * ring. Looking at decoder pending here would pace against the frame
     * beyond the newest retained one and skip the history on resume. */
    if (s->ring.back > 0) {
        int idx = s->ring.len - s->ring.back;
        if (idx >= 0 && idx < s->ring.len)
            return pts_to_sec(s, s->ring.buf[idx]);
    }
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

    s->frame_no = displayed_frame_no(s, f, s->frame_no + 1);
    s->frames_presented++;

    if (s->ring.cap > 0)
        ring_push(&s->ring, av_frame_clone(f));

    if (s->keep_previous) {
        av_frame_free(&s->previous);
        s->previous = s->shown;
    } else {
        av_frame_free(&s->shown);
    }
    s->shown = f;
}

bool source_advance_to(Source *s, double t)
{
    bool changed = false;

    /* A master-clock move can go backward after frame stepping or a seek.
     * Find the retained frame in effect at that instant. Previously only
     * the focused source moved backward; the other stayed in the future. */
    double shown_t = source_shown_sec(s);
    if (!isnan(shown_t) && shown_t > t + 1e-9) {
        int best = -1;
        for (int i = 0; i < s->ring.len; i++) {
            double ft = pts_to_sec(s, s->ring.buf[i]);
            if (!isnan(ft) && ft <= t + 1e-9) best = i;
        }
        if (best >= 0) {
            s->ring.back = s->ring.len - 1 - best;
            show_ring_current(s);
            s->eof = false;
            changed = true;
        } else {
            /* The target predates retained history. Decode forward from
             * the preceding keyframe to reconstruct the correct frame. */
            if (!decoder_seek_to(&s->dec, t)) return false;
            source_flush(s);
            changed = true;
        }
    }

    /* Walk retained frames according to their timestamps. Do not jump to
     * the newest frame merely because playback resumed. */
    while (s->ring.back > 0) {
        int idx = s->ring.len - s->ring.back;
        double next = pts_to_sec(s, s->ring.buf[idx]);
        if (!isnan(next) && next > t + 1e-9) break;
        ring_step_fwd(&s->ring);
        show_ring_current(s);
        changed = true;
    }

    /* More retained frames are still in the future; decoding pending is
     * not due yet. */
    if (s->ring.back > 0) return changed;

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

bool source_seek_to(Source *s, double t)
{
    if (t < 0.0) t = 0.0;
    if (!decoder_seek_to(&s->dec, t)) return false;
    source_flush(s);
    source_advance_to(s, t);
    return true;
}

double source_step_forward(Source *s)
{
    /* Walk back toward live before decoding anything new. */
    if (ring_step_fwd(&s->ring)) {
        if (ring_current(&s->ring)) {
            show_ring_current(s);
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
        s->eof = false;
        if (ring_current(&s->ring)) {
            show_ring_current(s);
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
