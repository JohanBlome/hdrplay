#ifndef HDRPLAY_DECODER_H
#define HDRPLAY_DECODER_H

#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

typedef struct {
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
    enum AVColorRange            range;
    enum AVPixelFormat           pix_fmt;
    int                          bit_depth;

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

bool  decoder_open(Decoder *d, const char *path);
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
