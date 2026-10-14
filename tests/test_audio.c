#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <libavutil/time.h>

static int fails;
int g_verbose = 0;
#define CHECK(c, name) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", name); fails++; } } while (0)
static void u16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void u32(FILE *f, unsigned v) { u16(f, v); u16(f, v >> 16); }
static void fixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) exit(1);
    unsigned n = 48000, bytes = n * 2 * 4;
    fwrite("RIFF", 1, 4, f); u32(f, bytes + 36); fwrite("WAVEfmt ", 1, 8, f);
    u32(f, 16); u16(f, 3); u16(f, 2); u32(f, 48000); u32(f, 48000 * 8);
    u16(f, 8); u16(f, 32); fwrite("data", 1, 4, f); u32(f, bytes);
    for (unsigned i = 0; i < n; i++) {
        float a = i == 12000 ? 0.5f : 0, b = i == 12048 ? -0.5f : 0;
        fwrite(&a, 4, 1, f); fwrite(&b, 4, 1, f);
    }
    fclose(f);
}
static bool ready(Audio *a)
{
    uint64_t deadline = SDL_GetTicks() + 5000;
    AudioInfo info;
    do { audio_info(a, &info); if (info.ready || info.failed) return info.ready && !info.failed; SDL_Delay(5); }
    while (SDL_GetTicks() < deadline);
    return false;
}
static double await_clock(Audio *a, double target)
{
    uint64_t deadline = SDL_GetTicks() + 3000;
    double t;
    do { t = audio_clock(a); if (t >= target) return t; SDL_Delay(5); }
    while (SDL_GetTicks() < deadline);
    return t;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        const char *paths[] = { argv[1] };
        Audio *a = audio_open(paths, 1, 0, 0, false, true, NULL);
        CHECK(a && ready(a), "timestamp fixture decodes");
        if (!a) return 1;
        if (argc > 2 && !strcmp(argv[2], "jitter")) {
            AudioInfo info; audio_info(a, &info);
            CHECK(info.timing_inferred && info.timestamp_uncertainty > .001,
                  "timestamp jitter recovered with explicit uncertainty");
            printf("decoded %.6f s; timestamp uncertainty %.3f ms\n", info.duration,
                   info.timestamp_uncertainty * 1000);
            audio_close(a); return fails != 0;
        }
        if (argc > 2 && !strcmp(argv[2], "gap")) {
            CHECK(!audio_marker_sample(a, .55, 0), "real audio gap has no sample to mark");
            float low[100], high[100]; audio_envelope(a, .54, .04, 100, 0, low, high);
            bool empty = true; for (int i = 0; i < 100; i++) if (isfinite(low[i])) empty = false;
            CHECK(empty, "real audio gap remains absent on waveform");
            AudioInfo info; audio_info(a, &info);
            CHECK(fabs(info.duration - 1.1) < .001, "gap is retained on presentation timeline");
            audio_close(a); return fails != 0;
        }
        if (argc > 2 && !strcmp(argv[2], "aac")) {
            /* AAC priming is handled by FFmpeg; compare against its decoded
             * PCM rather than assuming a lossy impulse remains sharp. */
            FILE *ref = fopen(argv[3], "rb");
            CHECK(ref != NULL, "AAC PCM reference exists");
            long bytes = 0;
            if (ref) { fseek(ref, 0, SEEK_END); bytes = ftell(ref); rewind(ref); }
            size_t frames = bytes > 0 ? (size_t)bytes / (sizeof(float) * 2) : 0;
            float *pcm = frames ? calloc(frames * 2, sizeof(float)) : NULL;
            size_t samples = ref && pcm ? fread(pcm, sizeof(float) * 2, frames, ref) : 0;
            if (ref) fclose(ref);
            AudioInfo info; audio_info(a, &info);
            CHECK(info.decoded_samples == (int64_t)samples, "sample audit matches complete FFmpeg reference");
            CHECK(samples >= 48000 && fabs(info.duration - (samples / 48000.0 + .001)) < .002,
                  "AAC priming and final padding match FFmpeg decoded reference");
            float low[192], high[192];
            audio_envelope(a, .241, .004, 192, 0, low, high);
            float expected = 0, actual = 0;
            for (int i = 11520; pcm && i < 11712; i++) expected = fmaxf(expected, pcm[i * 2]);
            for (int i = 0; i < 192; i++) if (isfinite(high[i])) actual = fmaxf(actual, high[i]);
            CHECK(fabs(actual - expected) < .0001, "AAC waveform aligns to source sample after codec priming");
            free(pcm); audio_close(a); return fails != 0;
        }
        AudioMarker m = {0};
        CHECK(audio_transient_near(a, 0.24, 0, &m, NULL), "fixture impulse detected");
        CHECK(fabs(m.time - 0.241) < 1.1 / 48000, "nonzero shared origin preserves 1ms A/V offset");
        double times[64]; int64_t ord[64]; int n;
        uint64_t deadline = SDL_GetTicks() + 5000;
        do { n = audio_frame_grid(a, 0, 0, 1, times, ord, 64); if (n >= 20) break; SDL_Delay(5); }
        while (SDL_GetTicks() < deadline);
        CHECK(n == 20, "presentation-order frame index is complete");
        if (n >= 11) {
            CHECK(fabs(times[6] - 0.24) < 0.00001 && ord[6] == 6, "actual frame timestamp and ordinal");
            CHECK(fabs(times[10] - 0.42) < 0.0011, "variable frame interval preserved");
        }
        audio_close(a);
        return fails != 0;
    }
    char wav[] = "/tmp/hdrplay-audio-test-XXXXXX";
    int fd = mkstemp(wav); if (fd < 0) return 1; close(fd); fixture(wav);
    char sidecar[] = "/tmp/hdrplay-marker-test-XXXXXX";
    fd = mkstemp(sidecar); if (fd < 0) return 1; close(fd); unlink(sidecar);
    const char *paths[] = { wav };
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Audio *a = audio_open(paths, 1, 0, 0, true, true, sidecar);
    CHECK(a && ready(a), "background PCM decode completes");
    if (!a) { unlink(wav); return 1; }
    AudioInfo info; audio_info(a, &info);
    CHECK(info.rate == 48000 && info.channels == 2, "source sample rate and channels");
    CHECK(info.decoded_samples == 48000 && !info.duration_mismatch && info.declared_duration == 1,
          "consistent PCM duration passes the length audit");
    CHECK(info.transients == 2, "automatic markers preserve separate channel arrivals");
    float low[100], high[100];
    audio_envelope(a, 0.249, 0.004, 100, 0, low, high);
    float peak = 0;
    for (int i = 0; i < 100; i++) if (isfinite(high[i])) peak = fmaxf(peak, high[i]);
    CHECK(peak == 0.5f, "sample waveform preserves short peak");
    audio_envelope(a, 0, 1, 100, 1, low, high);
    float minimum = 0;
    for (int i = 0; i < 100; i++) if (isfinite(low[i])) minimum = fminf(minimum, low[i]);
    CHECK(minimum == -0.5f, "coarse waveform preserves phase-inverted channel");
    CHECK(audio_marker_sample(a, 0.25 + 0.4 / 48000, 0), "manual sample marker");
    CHECK(audio_marker_time(a, 0.3), "absolute marker");
    AudioMarker markers[16]; int n = audio_markers(a, markers, 16);
    CHECK(n == 4 && markers[2].sample == 12000 && markers[2].time == 0.25, "sample snap is exact");
    CHECK(audio_marker_review(a, 0, true), "automatic marker dismissal");
    CHECK(audio_marker_label(a, 2, "clap \"onset\" \\ test"), "marker label with JSON escapes");
    CHECK(audio_markers_save(a), "atomic JSON save");
    audio_transport(a, 0.1, false, true);
    double t = await_clock(a, 0.2);
    CHECK(t >= 0.2 && t < 0.4, "audio clock follows device consumption");
    audio_transport(a, t, true, false);
    double held = audio_clock(a); SDL_Delay(80);
    CHECK(fabs(audio_clock(a) - held) < 1e-9, "pause freezes audio clock");
    for (int i = 0; i < 5; i++) {
        audio_transport(a, 0.05, true, true);
        CHECK(fabs(audio_clock(a) - 0.05) < 1e-9, "seek discards old queue and reanchors");
        audio_transport(a, 0.05, false, false);
        CHECK(await_clock(a, 0.1) < 0.25, "resume/loop returns to requested position");
    }
    audio_transport(a, 0.9, false, true);
    CHECK(await_clock(a, 1.05) >= 1.05, "audio EOF hands over to monotonic clock");
    audio_close(a);
    a = audio_open(paths, 1, 0, 0, false, true, sidecar);
    CHECK(a && ready(a), "JSON reload");
    if (a) {
        n = audio_markers(a, markers, 16);
        CHECK(n == 4 && markers[0].dismissed, "reanalyzing preserves dismissal without duplicates");
        CHECK(markers[2].sample == 12000 && !strcmp(markers[2].label, "clap \"onset\" \\ test"), "exact sample and escaped label round trip");
        audio_close(a);
    }
    FILE *f = fopen(wav, "ab"); fputc(0, f); fclose(f);
    a = audio_open(paths, 1, 0, 0, false, false, sidecar);
    CHECK(!a, "changed source rejects saved markers"); audio_close(a);
    unlink(wav); unlink(sidecar);
    printf("audio: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
