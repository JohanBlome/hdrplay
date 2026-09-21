#ifndef HDRPLAY_CLAMP_H
#define HDRPLAY_CLAMP_H

/* Clamp to the unit interval, in both precisions the codebase needs.
 *
 * Normalized quantities turn up all over: pan offsets in source units,
 * signal values out of the YUV->RGB matrix (which routinely lands just
 * outside [0,1] on saturated or out-of-gamut codes), the interpolation
 * parameter feeding a smoothstep. Spelling the two bounds checks out by
 * hand every time is what produced a dozen -Wmisleading-indentation
 * warnings; naming the operation says what it is, once.
 *
 * NaN passes through unchanged, exactly as it did in the hand-written
 * form — both comparisons are false. */
static inline float fclamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline double dclamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

#endif
