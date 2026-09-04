#ifndef HDRPLAY_DECODER_H
#define HDRPLAY_DECODER_H

#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

typedef struct Decoder {
    AVFormatContext *fmt;
    AVCodecContext  *cc;
    int              stream_idx;
    AVPacket        *pkt;
    AVFrame         *frame;

    /* Cached metadata extracted at open time, so the renderer and HUD
     * don't have to re-parse codecpar every frame. These describe what
     * the container CLAIMS — your existing vca.py verifies whether the
     * pixels back the claim. */
    int width, height;
    enum AVColorPrimaries        primaries;
    enum AVColorTransferCharacteristic transfer;
    enum AVColorSpace            matrix;
    enum AVColorRange            range;      /* as the container claims */
    enum AVPixelFormat           pix_fmt;
    int                          bit_depth;

    /* The range every decoded frame is stamped with. Equal to `range`
     * when the container declares one; otherwise recovered from the
     * pixels by decoder_resolve_color_range(), or left UNSPECIFIED
     * (which every consumer reads as limited) when that fails. */
    enum AVColorRange            range_effective;
    bool                         range_guessed;
    double                       range_outside_frac;

    /* HDR10 static metadata, if present in stream side data. */
    bool   has_mastering_display;
    double mdcv_red_x, mdcv_red_y;
    double mdcv_green_x, mdcv_green_y;
    double mdcv_blue_x, mdcv_blue_y;
    double mdcv_white_x, mdcv_white_y;
    double mdcv_min_luma, mdcv_max_luma;
    bool   has_cll;
    int    cll_max, cll_avg;
} Decoder;

/* Force the colour range for sources that do not declare one, or
 * override one that does. AVCOL_RANGE_UNSPECIFIED (the default) leaves
 * it to the container, then to pixel inspection. Wired to --range. */
void  decoder_set_range_override(enum AVColorRange r);

bool  decoder_open(Decoder *d, const char *path);

/* Recover the colour range of an untagged source from its pixels, then
 * rewind. No-op when the container declares a range, when --range set
 * one, or when the input cannot be seeked back (recovering the range
 * is not worth consuming the head of a pipe). Pools the excursion
 * counts over up to `max_frames` frames rather than trusting one,
 * which a fade-in or a title card would otherwise decide. Returns true
 * if a range was resolved. Call right after decoder_open(). */
bool  decoder_resolve_color_range(Decoder *d, int max_frames);

int   decoder_next_frame(Decoder *d);   /* >0 got frame, 0 EOF, <0 error */
/* Fold any per-frame HDR10 side data into the cached stream metadata.
 * Encoders that stamp MaxCLL/MaxFALL per frame rather than per stream
 * would otherwise look like they declare nothing. */
void  decoder_absorb_frame_side_data(Decoder *d);
bool  decoder_seek_start(Decoder *d);   /* rewind for --loop; false on err */
bool  decoder_seek_to(Decoder *d, double seconds);   /* seek to absolute time; clamps to >= 0 */
double decoder_frame_seconds(const Decoder *d);      /* current frame's PTS in seconds (NaN if none) */
void  decoder_close(Decoder *d);

#endif
