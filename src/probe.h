#ifndef HDRPLAY_PROBE_H
#define HDRPLAY_PROBE_H

#include <stdbool.h>
#include <libavutil/frame.h>

/* Result of sampling a single pixel from a source AVFrame. All "nits"
 * values are in cd/m² in the source's reference (i.e. what the pixel
 * data SAYS the scene luminance is, before any rendering / tone-map /
 * gamut-map happens on the GPU). This is the "ground truth" of the
 * file — useful for answering "is this red supposed to be visible?". */
typedef struct {
    int    src_x, src_y;         /* source pixel actually read */
    double y_norm;               /* normalized Y' (0..1)        */
    double r_norm, g_norm, b_norm; /* normalized RGB after YUV→RGB */
    double r_nits, g_nits, b_nits; /* linear cd/m² per channel    */
    double luma_nits;            /* BT.2020/709-weighted luma cd/m² */
} ProbeResult;

/* Sample (src_x, src_y) from `frame`. Only yuv420p, yuv420p10le, and
 * yuv420p12le are currently supported (covers ~all HDR10 and SDR clips
 * you'll throw at this tool). Returns false for other pixel formats. */
bool probe_sample(const AVFrame *frame, int src_x, int src_y,
                  ProbeResult *out);

#endif
