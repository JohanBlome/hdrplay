/* Frame-ring and sync tests.
 *
 * The ring is exercised directly with dummy AVFrames — no decoder, no
 * media — because its cursor semantics are where stepping goes wrong:
 * step back N then forward N must land on the same frames, and pushing
 * while parked in the past must not branch the history.
 *
 * The sync rule ("last frame whose PTS <= clock") is tested as pure
 * arithmetic over PTS lists, which is what actually determines whether
 * two files at different frame rates line up.
 */
#include "source.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <libavutil/frame.h>
#if HDRPLAY_HAVE_AMVE
#include <libavutil/ambient_viewing_environment.h>
#endif

static int fails = 0;
#define CHECK(cond, fmt, ...) do {                                  \
    if (cond) printf("  ok    " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); fails++; }   \
} while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

/* A frame identified only by its PTS — enough to track identity. */
static AVFrame *mkf(int64_t pts)
{
    AVFrame *f = av_frame_alloc();
    /* Give the frame refcounted storage so av_frame_clone(), used by Source
     * when changing its displayed ring entry, can clone it. */
    f->format = AV_PIX_FMT_GRAY8;
    f->width = f->height = 1;
    av_frame_get_buffer(f, 1);
    f->pts = pts;
    f->best_effort_timestamp = pts;
    return f;
}

static int64_t cur_pts(const FrameRing *r)
{
    AVFrame *f = ring_current(r);
    return f ? f->pts : -1;
}

/* ------------------------------------------------------------------ */
static void test_ring_basics(void)
{
    puts("ring: push, cursor, eviction");
    FrameRing r;
    ring_init(&r, 4);

    CHECK(ring_current(&r) == NULL, "empty ring has no current frame");
    CHECK(!ring_step_back(&r), "cannot step back on an empty ring");
    CHECK(!ring_step_fwd(&r), "cannot step forward on an empty ring");

    for (int i = 0; i < 3; i++) ring_push(&r, mkf(i));
    CHECK(r.len == 3, "3 frames held (got %d)", r.len);
    CHECK(cur_pts(&r) == 2, "newest is current (pts %lld)", (long long)cur_pts(&r));

    /* Eviction past capacity drops the oldest, not the newest. */
    for (int i = 3; i < 7; i++) ring_push(&r, mkf(i));
    CHECK(r.len == 4, "capped at 4 (got %d)", r.len);
    CHECK(cur_pts(&r) == 6, "newest still current after eviction");
    int steps = 0;
    while (ring_step_back(&r)) steps++;
    CHECK(steps == 3, "can walk back 3 from a 4-deep ring (got %d)", steps);
    CHECK(cur_pts(&r) == 3, "oldest retained is pts 3 (got %lld)",
          (long long)cur_pts(&r));

    ring_free(&r);
}

static void test_ring_step_roundtrip(void)
{
    puts("ring: step back N then forward N returns the same frames");
    FrameRing r;
    ring_init(&r, 8);
    for (int i = 0; i < 8; i++) ring_push(&r, mkf(i * 10));

    int64_t seen_back[8], seen_fwd[8];
    int nb = 0;
    seen_back[nb++] = cur_pts(&r);
    while (ring_step_back(&r)) seen_back[nb++] = cur_pts(&r);

    int nf = 0;
    seen_fwd[nf++] = cur_pts(&r);
    while (ring_step_fwd(&r)) seen_fwd[nf++] = cur_pts(&r);

    CHECK(nb == nf, "same number of steps each way (%d / %d)", nb, nf);
    bool mirror = true;
    for (int i = 0; i < nb && i < nf; i++)
        if (seen_back[i] != seen_fwd[nf - 1 - i]) mirror = false;
    CHECK(mirror, "forward walk is the exact reverse of the backward walk");
    CHECK(cur_pts(&r) == 70, "ends back at the newest frame");

    ring_free(&r);
}

static void test_ring_push_while_parked(void)
{
    puts("ring: pushing while parked in the past does not branch history");
    FrameRing r;
    ring_init(&r, 8);
    for (int i = 0; i < 5; i++) ring_push(&r, mkf(i));

    ring_step_back(&r);
    ring_step_back(&r);
    CHECK(cur_pts(&r) == 2, "parked at pts 2");

    /* Resuming playback from here must discard 3 and 4, not interleave. */
    ring_push(&r, mkf(99));
    CHECK(r.back == 0, "cursor returns live on push");
    CHECK(cur_pts(&r) == 99, "new frame is current");
    CHECK(r.len == 4, "history truncated to 0,1,2,99 (len %d)", r.len);
    ring_step_back(&r);
    CHECK(cur_pts(&r) == 2, "previous frame is 2, not the discarded 4");

    ring_free(&r);
}

static void test_ring_disabled(void)
{
    puts("ring: capacity 0 disables retention cleanly");
    FrameRing r;
    ring_init(&r, 0);
    ring_push(&r, mkf(1));      /* must free, not leak or crash */
    CHECK(r.len == 0, "nothing retained");
    CHECK(ring_current(&r) == NULL, "no current frame");
    CHECK(!ring_step_back(&r), "step back always fails, forcing the seek path");
    ring_free(&r);
}

static void test_temporal_previous(void)
{
    puts("source: temporal difference follows the displayed predecessor");
    Source s = {0};
    s.tb_sec = 1.0;
    s.fps = 1.0;
    ring_init(&s.ring, 8);
    for (int i = 0; i < 4; i++) ring_push(&s.ring, mkf(i));
    s.shown = av_frame_clone(ring_current(&s.ring));
    s.frame_no = 3;

    source_keep_previous(&s, true);
    CHECK(s.previous && s.previous->pts == 2,
          "enabling diff seeds previous from retained history");

    CHECK(!isnan(source_step_back(&s)), "step back succeeds");
    CHECK(s.shown && s.shown->pts == 2, "shown frame moves to pts 2");
    CHECK(s.frame_no == 2, "counter moves backward to frame 2");
    CHECK(s.previous && s.previous->pts == 1,
          "previous moves to the predecessor, not the frame stepped from");

    CHECK(!isnan(source_step_forward(&s)), "step forward succeeds");
    CHECK(s.shown && s.shown->pts == 3, "shown frame returns to pts 3");
    CHECK(s.frame_no == 3, "counter moves forward to frame 3");
    CHECK(s.previous && s.previous->pts == 2,
          "previous follows forward ring navigation");

    source_keep_previous(&s, false);
    CHECK(s.previous == NULL, "disabling diff releases the retained predecessor");
    av_frame_free(&s.shown);
    ring_free(&s.ring);
}

static void test_source_clock_alignment_in_history(void)
{
    puts("source: backward clock alignment keeps hidden panes synchronized");
    Source s = {0};
    s.tb_sec = 1.0;
    s.fps = 1.0;
    ring_init(&s.ring, 8);
    for (int i = 0; i < 5; i++) ring_push(&s.ring, mkf(i));
    s.shown = av_frame_clone(ring_current(&s.ring));
    s.frame_no = 4;
    s.eof = true; /* Rewinding from EOF must make retained frames playable. */

    CHECK(source_advance_to(&s, 1.5), "backward clock move changes the shown frame");
    CHECK(s.shown && s.shown->pts == 1, "t=1.5 selects retained pts 1");
    CHECK(s.frame_no == 1, "backward clock alignment updates frame counter");
    CHECK(!s.eof, "moving into retained history clears presentation EOF");
    CHECK(NEAR(source_peek_next_sec(&s), 2.0, 1e-9),
          "next timestamp comes from retained history, not decoder pending");

    CHECK(source_advance_to(&s, 3.2), "forward clock walks retained frames");
    CHECK(s.shown && s.shown->pts == 3, "t=3.2 selects retained pts 3");
    CHECK(s.frame_no == 3, "forward history walk updates frame counter");
    CHECK(NEAR(source_peek_next_sec(&s), 4.0, 1e-9),
          "pacing continues with the next retained frame");

    av_frame_free(&s.shown);
    ring_free(&s.ring);
}

/* The distinction that made a seek onto the last frame quit: the
 * decoder running dry is one peek AHEAD of the last frame leaving the
 * screen, so it cannot be the signal to stop. */
static void test_finished_holds_the_last_frame(void)
{
    puts("source: finishing waits for the last frame to play out");
    Source s = {0};
    s.tb_sec = 1.0;
    s.fps = 2.0;                /* 0.5s frames when the container is silent */
    s.shown = mkf(4);           /* last frame, pts 4s, no stated duration */

    CHECK(!source_finished(&s, 100.0),
          "a source with frames left is never finished, however late the clock");

    s.eof = true;
    CHECK(!source_finished(&s, 4.0), "not finished the instant the decoder dries up");
    CHECK(!source_finished(&s, 4.4), "still showing the last frame partway through");
    CHECK(source_finished(&s, 4.5), "finished once the last frame has had its 0.5s");

    /* A stated duration wins over the nominal rate, so a variable-rate
     * stream is not held for an interval it never used. */
    s.shown->duration = 2;      /* 2s in this timebase, not 1/fps */
    CHECK(!source_finished(&s, 5.9), "a stated frame duration outlasts the nominal rate");
    CHECK(source_finished(&s, 6.0), "finished at pts + stated duration");

    av_frame_free(&s.shown);

    /* Nothing was ever shown — there is no frame to wait out. */
    Source empty = {0};
    empty.tb_sec = 1.0;
    empty.eof = true;
    CHECK(source_finished(&empty, 0.0), "a source that showed nothing is finished at once");
}

static void test_seek_ceiling(void)
{
    puts("source: a forward seek clamps inside the last frame");
    /* 33 frames stamped 30fps but actually running at 30.006 — the real
     * shape of the clip that exposed this. The last frame starts at
     * 1.066456, which is PAST duration - 1/30 (1.066444). A whole-frame
     * backoff lands on frame 31 and the last frame is unreachable. */
    Source s[2];
    memset(s, 0, sizeof(s));
    s[0].duration_sec = 1.099778;
    s[0].fps = 30.0;
    double last_pts = 1.066456;

    double c = source_seek_ceiling(s, 1);
    CHECK(c >= last_pts, "ceiling %.6f reaches the last frame at %.6f", c, last_pts);
    CHECK(c < s[0].duration_sec, "and stays inside the clip");

    /* With two inputs the longer one decides, so the short one is not
     * cut off by a clamp derived from its own end. */
    s[1].duration_sec = 5.0;
    s[1].fps = 25.0;
    CHECK(source_seek_ceiling(s, 2) > 4.9, "the longest input sets the ceiling");

    /* A source with no declared duration must not contribute a guess. */
    Source unknown = {0};
    unknown.fps = 30.0;
    CHECK(source_seek_ceiling(&unknown, 1) < 0.0,
          "no duration means no clamp, not a clamp to zero");

    /* A duration shorter than the backoff must not go negative. */
    Source tiny = {0};
    tiny.duration_sec = 0.001;
    tiny.fps = 30.0;
    CHECK(source_seek_ceiling(&tiny, 1) == 0.0, "a sub-frame clip clamps to 0");
}

#if HDRPLAY_HAVE_AMVE
static void test_ambient_viewing_metadata(void)
{
    puts("decoder: per-frame ambient viewing environment metadata");
    Decoder d = {0};
    d.frame = av_frame_alloc();
    AVFrameSideData *sd = av_frame_new_side_data(
        d.frame, AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT,
        sizeof(AVAmbientViewingEnvironment));
    CHECK(sd != NULL, "AMVE side data allocated");
    if (sd) {
        AVAmbientViewingEnvironment *a =
            (AVAmbientViewingEnvironment *)sd->data;
        a->ambient_illuminance = (AVRational){ 3140000, 10000 };
        a->ambient_light_x     = (AVRational){ 15635, 50000 };
        a->ambient_light_y     = (AVRational){ 16450, 50000 };
        decoder_absorb_frame_side_data(&d);
        CHECK(d.has_ambient_viewing, "AMVE presence retained");
        CHECK(NEAR(d.ambient_illuminance_lux, 314.0, 1e-9),
              "AMVE illuminance is %.1f lux", d.ambient_illuminance_lux);
        CHECK(NEAR(d.ambient_light_x, 0.3127, 1e-9) &&
              NEAR(d.ambient_light_y, 0.3290, 1e-9),
              "AMVE white is (%.4f, %.4f)",
              d.ambient_light_x, d.ambient_light_y);
    }
    av_frame_free(&d.frame);
}
#endif

/* ------------------------------------------------------------------ */
/* The sync rule, as arithmetic. Given a clock and a list of frame
 * times, the frame in effect is the last one whose PTS <= clock. */
static int frame_in_effect(const double *pts, int n, double clock)
{
    int idx = -1;
    for (int i = 0; i < n; i++) if (pts[i] <= clock + 1e-9) idx = i;
    return idx;
}

static void test_sync_rule(void)
{
    puts("sync: 24fps against 30fps on one clock");
    double a[64], b[64];
    for (int i = 0; i < 64; i++) { a[i] = i / 24.0; b[i] = i / 30.0; }

    /* Same instant, different indices — the whole point of a PTS clock
     * rather than frame-index lockstep. */
    int ia = frame_in_effect(a, 64, 1.0);
    int ib = frame_in_effect(b, 64, 1.0);
    CHECK(ia == 24 && ib == 30, "at t=1.0s: A frame %d, B frame %d", ia, ib);

    ia = frame_in_effect(a, 64, 0.5);
    ib = frame_in_effect(b, 64, 0.5);
    CHECK(ia == 12 && ib == 15, "at t=0.5s: A frame %d, B frame %d", ia, ib);

    /* Between frames, the earlier one is still in effect. */
    ia = frame_in_effect(a, 64, 1.0 / 24.0 - 0.001);
    CHECK(ia == 0, "just before A's second frame, frame 0 holds");

    /* Frame-index lockstep would drift; show the size of the error. */
    double drift = a[24] - b[24];
    CHECK(fabs(drift - 0.2) < 1e-9,
          "index lockstep would put them %.3fs apart at frame 24", drift);

    /* A shorter file holds its last frame rather than going black. */
    double shortb[10];
    for (int i = 0; i < 10; i++) shortb[i] = i / 30.0;
    int is = frame_in_effect(shortb, 10, 5.0);
    CHECK(is == 9, "past EOF the last frame stays in effect (got %d)", is);

    /* Before the first frame, nothing is in effect yet. */
    CHECK(frame_in_effect(a, 64, -1.0) == -1, "before the start, no frame");
}

int main(void)
{
    test_ring_basics();
    test_ring_step_roundtrip();
    test_ring_push_while_parked();
    test_ring_disabled();
    test_temporal_previous();
    test_source_clock_alignment_in_history();
    test_finished_holds_the_last_frame();
    test_seek_ceiling();
#if HDRPLAY_HAVE_AMVE
    test_ambient_viewing_metadata();
#endif
    test_sync_rule();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
