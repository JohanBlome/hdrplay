#ifndef HDRPLAY_AUDIO_INTERNAL_H
#define HDRPLAY_AUDIO_INTERNAL_H
#include "audio.h"
#include "transient.h"
#include <stdio.h>
#include <sys/stat.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

#define AUDIO_PEAK_LEVELS 9
#define AUDIO_CACHE_LIMIT ((int64_t)4 * 1024 * 1024 * 1024)
#define AUDIO_INDEX_LIMIT 2000000
#define AUDIO_PEAK_LIMIT 524288
#define AUDIO_PEAK_SAMPLES 512
typedef struct {
    double time, end;
    int64_t sample, pts, offset;
    int count;
    AVRational tb;
} AudioBlock;
typedef struct {
    double time, end;
    int first, last;
    float low[AUDIO_MAX_CHANNELS], high[AUDIO_MAX_CHANNELS];
} AudioPeak;
typedef struct { AudioPeak *p; int n, cap; } AudioPeaks;
typedef struct { double time, end; int64_t pts; } AudioFrame;
typedef struct {
    char *path;
    struct stat identity;
    double origin;
    AudioFrame *frames;
    int n_frames, cap_frames, stream;
    bool complete, failed;
    double declared_file_duration, declared_file_end;
    bool file_end_known, other_tracks;
} AudioInput;
struct Audio {
    SDL_Mutex *mutex;
    SDL_Condition *changed;
    SDL_AtomicInt stop;
    SDL_Thread *decoder_thread, *index_thread, *output_thread;
    AVFormatContext *fmt;
    AVCodecContext *cc;
    SwrContext *swr;
    int input, stream, rate, channels, n_inputs;
    AudioInput inputs[2];
    FILE *cache;
    int fd;
    int64_t bytes, samples;
    AudioBlock *blocks;
    int n_blocks, cap_blocks;
    AudioPeaks peaks[AUDIO_PEAK_LEVELS];
    AudioMarker markers[AUDIO_MAX_MARKERS];
    int n_markers;
    char *markers_path;
    bool detect, finished, failed, sound, paused, buffering, started, muted;
    bool output_failed;
    bool timing_inferred;
    double timestamp_uncertainty, jitter_window;
    int64_t priming_samples, padding_samples;
    double declared_duration, presentation_span, gap_duration, raw_end;
    double duration_delta;
    bool duration_mismatch;
    bool file_audit_complete, file_duration_mismatch;
    double file_observed_end, file_duration_delta;
    char error[160], channel_names[AUDIO_MAX_CHANNELS][24];
    double duration, target, cursor, last_clock, fallback_wall, fallback_time;
    double latency, device_block;
    uint64_t generation, revision;
    SDL_AudioStream *output;
};
bool audio_markers_load(Audio *a);
bool audio_markers_write(Audio *a);
/* Mutex must be held. */
bool audio_append_marker(Audio *a, const AudioMarker *marker);

#endif
