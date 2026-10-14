#ifndef HDRPLAY_AUDIO_H
#define HDRPLAY_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include <libavutil/rational.h>
#include "layout.h"

#define AUDIO_MAX_CHANNELS 8
#define AUDIO_MAX_MARKERS 4096
typedef struct Audio Audio;
typedef struct {
    double time, end;
    int64_t pts, sample, frame;
    int64_t detected_sample;
    int detected_channel;
    AVRational tb;
    int input, stream, channel;
    enum { AUDIO_MARK_SAMPLE, AUDIO_MARK_FRAME, AUDIO_MARK_TIME,
           AUDIO_MARK_TRANSIENT } kind;
    bool reviewed, dismissed;
    double uncertainty;
    double timestamp_uncertainty;
    float peak, contrast_db;
    char label[64];
} AudioMarker;
typedef struct {
    double start, span, clock;
    bool follow;
    bool logarithmic;
    bool dragging;
    double drag_x;
    int channel, selected, anchor;
    double cursor;
    int entry_mode; /* 1 = absolute time, 2 = label */
    char entry[64];
    int win_w, win_h;
    LayoutRect rect;
} AudioView;
typedef struct {
    int rate, channels, transients, markers;
    bool ready, failed, buffering, playing;
    bool timing_inferred;
    double timestamp_uncertainty;
    int64_t decoded_samples, priming_samples, padding_samples;
    double sample_duration, presentation_span, gap_duration, declared_duration;
    double duration_delta;
    bool duration_mismatch;
    bool file_audit_complete, file_duration_mismatch;
    double declared_file_duration, file_observed_end, file_duration_delta;
    double decoded_end, duration, latency;
    uint64_t revision;
    char error[160], channel_names[AUDIO_MAX_CHANNELS][24];
} AudioInfo;

Audio *audio_open(const char *const *paths, int n, int input, int track,
                  bool sound, bool detect, const char *markers_path);
void audio_close(Audio *a);
void audio_info(Audio *a, AudioInfo *info);
/* Explicit transport commands invalidate queued PCM before returning. */
void audio_transport(Audio *a, double time, bool paused, bool seek);
double audio_clock(Audio *a); /* audio-consumption estimate, NAN on failure */
void audio_mute(Audio *a, bool muted);
bool audio_marker_add(Audio *a, const AudioMarker *marker);
bool audio_marker_sample(Audio *a, double time, int channel);
bool audio_marker_time(Audio *a, double time);
bool audio_marker_frame(Audio *a, int input, int stream, int64_t pts,
                        AVRational tb, double time, double end);
int audio_markers(Audio *a, AudioMarker *out, int cap);
bool audio_marker_review(Audio *a, int index, bool dismiss);
bool audio_marker_label(Audio *a, int index, const char *label);
bool audio_marker_move(Audio *a, int index, double time, int channel);
bool audio_markers_save(Audio *a);
bool audio_transient_near(Audio *a, double time, int direction,
                          AudioMarker *out, int *index);
/* Fill pixel envelopes from source PCM/peaks. NaN means missing data. */
void audio_envelope(Audio *a, double start, double span, int width, int channel,
                     float *min_out, float *max_out);
/* Actual PTS boundaries; ordinal is only returned for a verified index entry. */
int audio_frame_grid(Audio *a, int input, double start, double end,
                      double *times, int64_t *ordinals, int cap);
int64_t audio_frame_ordinal(Audio *a, int input, int64_t pts);
double audio_input_origin(Audio *a, int input);

#endif
