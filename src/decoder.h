#ifndef HDRPLAY_DECODER_H
#define HDRPLAY_DECODER_H

#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/version.h>

/* AVAmbientViewingEnvironment and its AVFrame side-data tag arrived in
 * libavutil 57.44.100 (FFmpeg 6). Keep hdrplay buildable against older
 * distro FFmpeg packages while exposing one feature switch to the tests. */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 44, 100)
#define HDRPLAY_HAVE_AMVE 1
#else
#define HDRPLAY_HAVE_AMVE 0
#endif

typedef struct Decoder {
    AVFormatContext *fmt;
    AVCodecContext  *cc;
    int              stream_idx;
    /* Frames returned since the last rewind, i.e. the index of the frame
     * decoder_next_frame() will hand back next; -1 when a timestamp seek
     * has made it meaningless. The position of record for elementary
     * streams, which have no timestamps to hold one. */
    int64_t          frame_index;
    AVPacket        *pkt;
    AVFrame         *frame;       /* software frame exposed to consumers */
    AVFrame         *hw_frame;    /* decoder-owned hardware surface       */

    /* Hardware decode is selected at open time. decoder_next_frame downloads
     * hardware surfaces into `frame`, keeping the renderer and pixel probes
     * on one portable software-frame interface. */
    enum AVPixelFormat hw_pix_fmt;
    const char        *hw_name;
    bool               hw_requested;
    bool               hw_active;
    bool               hw_format_logged;

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

    /* H.274 / ISOBMFF `amve`: the nominal environment assumed when the
     * content was authored. This is descriptive metadata, not the viewer's
     * current ambient-light measurement. */
    bool   has_ambient_viewing;
    double ambient_illuminance_lux;
    double ambient_light_x, ambient_light_y;
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
/* Fold per-frame HDR/AMVE side data into the cached stream metadata.
 * Encoders and demuxers do not expose all metadata at the same level. */
void  decoder_absorb_frame_side_data(Decoder *d);
/* Rewind to the first frame. Elementary streams (AVFMT_NOTIMESTAMPS:
 * Annex-B .h264/.h265 and friends) are byte-seeked, because asking them
 * for a timestamp makes FFmpeg scan the entire file and then park the
 * demuxer at EOF. False on error. */
bool  decoder_seek_start(Decoder *d);
/* Seek to an absolute time, clamped to >= 0. On elementary streams the
 * time is converted to a frame index and reached by decoding forward
 * from the nearest earlier position, so the cost is linear in the
 * distance moved. */
bool  decoder_seek_to(Decoder *d, double seconds);
double decoder_frame_seconds(const Decoder *d);      /* current frame's PTS in seconds (NaN if none) */
void  decoder_close(Decoder *d);

#endif
