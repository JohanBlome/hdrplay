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
#include <math.h>
#include <libavutil/frame.h>

static int fails = 0;
#define CHECK(cond, fmt, ...) do {                                  \
    if (cond) printf("  ok    " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); fails++; }   \
} while (0)

/* A frame identified only by its PTS — enough to track identity. */
static AVFrame *mkf(int64_t pts)
{
    AVFrame *f = av_frame_alloc();
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
    test_sync_rule();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
