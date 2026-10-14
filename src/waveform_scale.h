#ifndef HDRPLAY_WAVEFORM_SCALE_H
#define HDRPLAY_WAVEFORM_SCALE_H
#include <math.h>
#include <stdbool.h>

/* Continuous signed log: silence stays at zero, full scale stays at one.
 * This changes display coordinates only, never PCM or onset detection. */
static inline float waveform_amplitude(float sample, bool logarithmic)
{
    float value = fmaxf(-1, fminf(1, sample));
    return logarithmic ? copysignf(log1pf(999 * fabsf(value)) / logf(1000), value) : value;
}
#endif
