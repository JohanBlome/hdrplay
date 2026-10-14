#include "transient.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define RATE 48000
#define N (RATE / 2)
static int fails;
#define CHECK(c, name) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", name); fails++; } } while (0)
typedef struct { TransientEvent e[16]; int n; } Events;
static void event(void *opaque, const TransientEvent *e)
{ Events *out = opaque; if (out->n < 16) out->e[out->n++] = *e; }
static Events run(const float *pcm, int channels, int chunk)
{
    Events out = {0};
    TransientDetector *d = transient_create(RATE, channels);
    CHECK(d != NULL, "detector allocation");
    for (int i = 0; i < N; i += chunk) {
        int n = N - i < chunk ? N - i : chunk;
        transient_feed(d, pcm + i * channels, n, event, &out);
    }
    transient_destroy(d); return out;
}
int main(void)
{
    float *pcm = calloc(N * 2, sizeof(float));
    if (!pcm) return 1;
    int onset = RATE / 5 + 17;
    pcm[onset * 2] = 0.5f;
    pcm[(onset + 48) * 2 + 1] = -0.5f;
    Events a = run(pcm, 2, 1), b = run(pcm, 2, 127), c = run(pcm, 2, 1024);
    CHECK(a.n == 2, "clean impulses in separate channels are detected");
    CHECK(a.n == b.n && a.n == c.n, "chunk size does not change event count");
    if (a.n == 2 && b.n == 2 && c.n == 2) {
        CHECK(a.e[0].sample == onset && a.e[1].sample == onset + 48, "onset and stereo arrival delay preserved");
        CHECK(a.e[0].sample == b.e[0].sample && a.e[1].sample == c.e[1].sample, "sample timing invariant across chunks");
    }
    memset(pcm, 0, N * 2 * sizeof(float));
    pcm[onset] = 0.000001f;
    CHECK(run(pcm, 1, 1024).n == 0, "tiny spike in silence rejected");
    for (int i = onset; i < N; i++) pcm[i] = 0.5f * sinf((float)(i - onset) * 2 * 3.14159265f * 1000 / RATE);
    CHECK(run(pcm, 1, 127).n == 0, "abrupt sustained tone rejected");
    memset(pcm, 0, N * 2 * sizeof(float));
    for (int i = 0; i < N; i++) pcm[i] = 0.03f * sinf(i * 0.2f);
    pcm[onset] = 0.14f;
    CHECK(run(pcm, 1, 1024).n == 0, "insufficient background contrast rejected");
    memset(pcm, 0, N * 2 * sizeof(float));
    pcm[onset] = pcm[onset + RATE / 200] = 0.5f;
    CHECK(run(pcm, 1, 127).n == 0, "ambiguous double attack rejected");
    memset(pcm, 0, N * 2 * sizeof(float));
    for (int i = onset; i < onset + 8; i++) pcm[i] = 1;
    CHECK(run(pcm, 1, 127).n == 0, "clipped impulse rejected");
    memset(pcm, 0, N * 2 * sizeof(float));
    pcm[onset] = 0.5f;
    TransientDetector *d = transient_create(RATE, 1);
    Events out = {0};
    transient_feed(d, pcm, onset, event, &out);
    transient_reset(d, onset);
    transient_feed(d, pcm + onset, N - onset, event, &out);
    CHECK(out.n == 0, "no strike inferred at a discontinuity without baseline");
    transient_destroy(d);
    free(pcm);
    printf("transient: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
