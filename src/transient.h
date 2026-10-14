#ifndef HDRPLAY_TRANSIENT_H
#define HDRPLAY_TRANSIENT_H

#include <stdbool.h>
#include <stdint.h>

#define TRANSIENT_VERSION 1
typedef struct {
    int64_t sample;
    int channel;
    float peak, contrast_db;
    int uncertainty_samples;
} TransientEvent;
typedef void (*TransientCallback)(void *, const TransientEvent *);
typedef struct TransientDetector TransientDetector;

TransientDetector *transient_create(int rate, int channels);
void transient_destroy(TransientDetector *d);
/* First sample index is independent of chunk size. Discontinuities reset
 * history, and must be explicitly passed via transient_reset(). */
void transient_reset(TransientDetector *d, int64_t first_sample);
void transient_feed(TransientDetector *d, const float *pcm, int samples,
                    TransientCallback callback, void *userdata);

#endif
