#include "decoder.h"
#include "log.h"
#include "probe.h"

#include <math.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavutil/mastering_display_metadata.h>
#if HDRPLAY_HAVE_AMVE
#include <libavutil/ambient_viewing_environment.h>
#endif

static const char *prim_name(enum AVColorPrimaries p)        { return av_color_primaries_name(p) ?: "unknown"; }
static const char *trc_name(enum AVColorTransferCharacteristic t) { return av_color_transfer_name(t) ?: "unknown"; }
static const char *mat_name(enum AVColorSpace s)             { return av_color_space_name(s) ?: "unknown"; }
static const char *rng_name(enum AVColorRange r) {
    return r == AVCOL_RANGE_JPEG ? "full" : r == AVCOL_RANGE_MPEG ? "limited" : "unspec";
}

static void log_metadata(const Decoder *d)
{
    LOG("DEC",  "stream: %dx%d, pix_fmt=%s, %d-bit",
        d->width, d->height, av_get_pix_fmt_name(d->pix_fmt) ?: "?", d->bit_depth);
    LOG("META", "primaries=%s transfer=%s matrix=%s range=%s",
        prim_name(d->primaries), trc_name(d->transfer), mat_name(d->matrix), rng_name(d->range));

    /* The "is this really HDR?" sniff test — duplicated from vca.py logic. */
    bool tagged_hdr = (d->transfer == AVCOL_TRC_SMPTE2084 ||  /* PQ  */
                       d->transfer == AVCOL_TRC_ARIB_STD_B67); /* HLG */
    LOG("META", "tagged_hdr=%s  (transfer=%s, bit_depth=%d)",
        tagged_hdr ? "YES" : "no", trc_name(d->transfer), d->bit_depth);
    if (tagged_hdr && d->bit_depth < 10)
        LOG("META", "WARNING: HDR transfer with <10-bit depth — likely mislabel");

    if (d->has_mastering_display) {
        LOG("META", "mastering display luma: %.4f .. %.0f nits",
            d->mdcv_min_luma, d->mdcv_max_luma);
        LOG("META", "mastering display primaries: R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) W(%.4f,%.4f)",
            d->mdcv_red_x, d->mdcv_red_y, d->mdcv_green_x, d->mdcv_green_y,
            d->mdcv_blue_x, d->mdcv_blue_y, d->mdcv_white_x, d->mdcv_white_y);
    } else {
        /* Scoped deliberately: this reads STREAM side data, and x265 among others
         * stamps the mastering display per frame instead. Saying a flat "none" here
         * contradicts the report, which does find it once decoding starts — and for
         * HLG that value sets L_W, so the two disagreeing is not cosmetic. */
        LOG("META", "no mastering display metadata in stream side data "
                    "(encoders that stamp it per frame are picked up on decode)");
    }
    if (d->has_cll)
        LOG("META", "content light: MaxCLL=%d MaxFALL=%d", d->cll_max, d->cll_avg);
    else
        LOG("META", "no MaxCLL/MaxFALL");

    if (d->has_ambient_viewing)
        LOG("META", "ambient viewing environment: %.1f lux, white=(%.5f,%.5f)",
            d->ambient_illuminance_lux,
            d->ambient_light_x, d->ambient_light_y);
    else if (d->transfer == AVCOL_TRC_ARIB_STD_B67)
        LOG("META", "no ambient viewing environment metadata");
}

#if HDRPLAY_HAVE_AMVE
static bool absorb_amve(Decoder *d, const uint8_t *data, size_t size)
{
    if (!data || size < sizeof(AVAmbientViewingEnvironment)) return false;

    const AVAmbientViewingEnvironment *a =
        (const AVAmbientViewingEnvironment *)data;
    d->has_ambient_viewing = true;
    d->ambient_illuminance_lux = a->ambient_illuminance.den
                               ? av_q2d(a->ambient_illuminance) : 0.0;
    d->ambient_light_x = a->ambient_light_x.den
                       ? av_q2d(a->ambient_light_x) : 0.0;
    d->ambient_light_y = a->ambient_light_y.den
                       ? av_q2d(a->ambient_light_y) : 0.0;
    return true;
}
#endif

/* HDR10 static and ambient-viewing metadata can live on the stream. Some
 * encoders also stamp it on each AVFrame; read codecpar first, then accept
 * per-frame upgrades in the decode path. */
static void extract_stream_sidedata(Decoder *d, const AVStream *st)
{
#if LIBAVCODEC_VERSION_MAJOR >= 60
    const AVPacketSideData *sd_mdcv = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    const AVPacketSideData *sd_cll = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
#if HDRPLAY_HAVE_AMVE && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 39, 100)
    const AVPacketSideData *sd_amve = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_AMBIENT_VIEWING_ENVIRONMENT);
#endif
#else
    /* Older ffmpeg: iterate codecpar->coded_side_data manually. */
    const AVPacketSideData *sd_mdcv = NULL, *sd_cll = NULL;
#if HDRPLAY_HAVE_AMVE && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 39, 100)
    const AVPacketSideData *sd_amve = NULL;
#endif
    for (int i = 0; i < st->codecpar->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &st->codecpar->coded_side_data[i];
        if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) sd_mdcv = sd;
        if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL)        sd_cll  = sd;
#if HDRPLAY_HAVE_AMVE && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 39, 100)
        if (sd->type == AV_PKT_DATA_AMBIENT_VIEWING_ENVIRONMENT) sd_amve = sd;
#endif
    }
#endif

    if (sd_mdcv && sd_mdcv->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
        const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)sd_mdcv->data;
        d->has_mastering_display = true;
        if (m->has_primaries) {
            d->mdcv_red_x   = av_q2d(m->display_primaries[0][0]);
            d->mdcv_red_y   = av_q2d(m->display_primaries[0][1]);
            d->mdcv_green_x = av_q2d(m->display_primaries[1][0]);
            d->mdcv_green_y = av_q2d(m->display_primaries[1][1]);
            d->mdcv_blue_x  = av_q2d(m->display_primaries[2][0]);
            d->mdcv_blue_y  = av_q2d(m->display_primaries[2][1]);
            d->mdcv_white_x = av_q2d(m->white_point[0]);
            d->mdcv_white_y = av_q2d(m->white_point[1]);
        }
        if (m->has_luminance) {
            d->mdcv_min_luma = av_q2d(m->min_luminance);
            d->mdcv_max_luma = av_q2d(m->max_luminance);
        }
    }

    if (sd_cll && sd_cll->size >= (int)sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata *c = (const AVContentLightMetadata *)sd_cll->data;
        d->has_cll = true;
        d->cll_max = c->MaxCLL;
        d->cll_avg = c->MaxFALL;
    }

#if HDRPLAY_HAVE_AMVE && LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 39, 100)
    if (sd_amve)
        absorb_amve(d, sd_amve->data, sd_amve->size);
#endif
}

/* Per-frame upgrade of the HDR10 static metadata.
 *
 * The comment above promises this, but nothing used to do it: some
 * encoders (x265 among them) stamp MaxCLL/MaxFALL on each AVFrame
 * rather than on the stream, so files carrying it that way looked like
 * they declared nothing at all. Call after every successful decode so
 * playback and --analyze agree on what the container claims. */
void decoder_absorb_frame_side_data(Decoder *d)
{
    if (!d || !d->frame) return;

    bool had_ambient_viewing = d->has_ambient_viewing;

    const AVFrameSideData *sd = av_frame_get_side_data(d->frame,
        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sd && sd->size >= sizeof(AVContentLightMetadata)) {
        const AVContentLightMetadata *c =
            (const AVContentLightMetadata *)sd->data;
        if (c->MaxCLL  > 0) { d->cll_max = c->MaxCLL;  d->has_cll = true; }
        if (c->MaxFALL > 0) { d->cll_avg = c->MaxFALL; d->has_cll = true; }
    }

    sd = av_frame_get_side_data(d->frame,
        AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sd && sd->size >= sizeof(AVMasteringDisplayMetadata)) {
        const AVMasteringDisplayMetadata *m =
            (const AVMasteringDisplayMetadata *)sd->data;
        if (m->has_luminance && m->max_luminance.den) {
            d->has_mastering_display = true;
            d->mdcv_min_luma = av_q2d(m->min_luminance);
            d->mdcv_max_luma = av_q2d(m->max_luminance);
        }
    }

#if HDRPLAY_HAVE_AMVE
    sd = av_frame_get_side_data(d->frame,
        AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT);
    if (sd) absorb_amve(d, sd->data, sd->size);
    if (!had_ambient_viewing && d->has_ambient_viewing)
        LOG("META", "ambient viewing environment: %.1f lux, white=(%.5f,%.5f) "
                    "(from decoded frame)",
            d->ambient_illuminance_lux,
            d->ambient_light_x, d->ambient_light_y);
#else
    (void)had_ambient_viewing;
#endif
}

/* --range, applied to every input. A file-static rather than a
 * per-open argument for the same reason probe's HLG peak is: it is a
 * one-shot startup decision, and threading it through every caller
 * buys nothing. */
static enum AVColorRange g_range_override = AVCOL_RANGE_UNSPECIFIED;

void decoder_set_range_override(enum AVColorRange r)
{
    g_range_override = r;
}

static enum AVPixelFormat choose_hw_format(AVCodecContext *cc,
                                            const enum AVPixelFormat *formats)
{
    Decoder *d = cc->opaque;
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == d->hw_pix_fmt) {
            d->hw_active = true;
            LOG("DEC", "hardware decode active: %s", d->hw_name);
            return *p;
        }
    }

    /* A codec can advertise a device globally yet reject a particular profile
     * or bitstream. Pick its software format instead of failing the open. */
    LOG("DEC", "WARNING: %s rejected this stream; using software decode",
        d->hw_name);
    d->hw_active = false;
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return AV_PIX_FMT_NONE;
}

/* Configure the platform decoder when the selected codec exposes it. This is
 * deliberately best-effort: unusual H.264/HEVC profiles and codecs that
 * VideoToolbox does not implement must remain playable. */
static bool configure_hardware_decode(Decoder *d, const AVCodec *codec)
{
#ifdef __APPLE__
    static const enum AVHWDeviceType preferred[] = {
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
    };
#elif defined(__linux__)
    /* VAAPI covers Intel and AMD through Mesa; CUDA covers NVIDIA when
     * FFmpeg was built with nv-codec-headers (as RPM Fusion's build is). */
    static const enum AVHWDeviceType preferred[] = {
        AV_HWDEVICE_TYPE_VAAPI,
        AV_HWDEVICE_TYPE_CUDA,
    };
#else
    (void)d;
    (void)codec;
    return false;
#endif

#if defined(__APPLE__) || defined(__linux__)
    for (size_t t = 0; t < sizeof(preferred) / sizeof(preferred[0]); t++) {
        const enum AVHWDeviceType type = preferred[t];
        const AVCodecHWConfig *match = NULL;
        for (int i = 0;; i++) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if (cfg->device_type == type &&
                (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                match = cfg;
                break;
            }
        }
        if (!match) continue;

        AVBufferRef *device = NULL;
        int err = av_hwdevice_ctx_create(&device, type, NULL, NULL, 0);
        if (err < 0) {
            char msg[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(err, msg, sizeof(msg));
            LOG("DEC", "%s unavailable: %s",
                av_hwdevice_get_type_name(type), msg);
            continue;
        }

        d->hw_pix_fmt = match->pix_fmt;
        d->hw_name = av_hwdevice_get_type_name(type);
        d->hw_requested = true;
        d->cc->opaque = d;
        d->cc->get_format = choose_hw_format;
        d->cc->hw_device_ctx = device; /* AVCodecContext owns this reference. */
        LOG("DEC", "hardware decode requested: %s", d->hw_name);
        return true;
    }
#endif
    return false;
}

static bool allocate_codec_context(Decoder *d, const AVCodec *codec,
                                   const AVCodecParameters *params,
                                   bool try_hardware)
{
    d->cc = avcodec_alloc_context3(codec);
    if (!d->cc || avcodec_parameters_to_context(d->cc, params) < 0)
        return false;

    /* avcodec_alloc_context3 defaults to one thread. Even the hardware
     * fallback must not silently regress to single-threaded decode. */
    d->cc->thread_count = 0;
    d->cc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (try_hardware)
        configure_hardware_decode(d, codec);
    return true;
}

bool decoder_open(Decoder *d, const char *path)
{
    memset(d, 0, sizeof(*d));
    d->hw_pix_fmt = AV_PIX_FMT_NONE;

    if (avformat_open_input(&d->fmt, path, NULL, NULL) < 0) {
        LOG("DEC", "ERROR: cannot open %s", path);
        return false;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        LOG("DEC", "ERROR: cannot read stream info");
        return false;
    }

    d->stream_idx = av_find_best_stream(d->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (d->stream_idx < 0) { LOG("DEC", "ERROR: no video stream"); return false; }

    AVStream *st = d->fmt->streams[d->stream_idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { LOG("DEC", "ERROR: no decoder for codec id %d", st->codecpar->codec_id); return false; }

    if (!allocate_codec_context(d, codec, st->codecpar, true)) {
        LOG("DEC", "ERROR: cannot allocate decoder");
        return false;
    }
    int open_err = avcodec_open2(d->cc, codec, NULL);
    if (open_err < 0 && d->hw_requested) {
        /* Device setup can succeed while codec initialization fails for a
         * profile VideoToolbox does not support. Recreate a clean software
         * context; an opened codec context cannot safely be reused. */
        char msg[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(open_err, msg, sizeof(msg));
        LOG("DEC", "WARNING: %s open failed: %s; using software decode",
            d->hw_name, msg);
        avcodec_free_context(&d->cc);
        d->hw_requested = false;
        d->hw_active = false;
        d->hw_pix_fmt = AV_PIX_FMT_NONE;
        if (!allocate_codec_context(d, codec, st->codecpar, false))
            return false;
        open_err = avcodec_open2(d->cc, codec, NULL);
    }
    if (open_err < 0) {
        char msg[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(open_err, msg, sizeof(msg));
        LOG("DEC", "ERROR: cannot open decoder: %s", msg);
        return false;
    }

    if (!d->hw_requested)
        LOG("DEC", "software decode: %d threads", d->cc->thread_count);

    d->width     = d->cc->width;
    d->height    = d->cc->height;
    d->primaries = d->cc->color_primaries;
    d->transfer  = d->cc->color_trc;
    d->matrix    = d->cc->colorspace;
    d->range     = d->cc->color_range;
    d->pix_fmt   = d->cc->pix_fmt;

    /* An explicit --range beats the container; otherwise the container
     * stands, and an absent flag is left for the pixel probe. */
    d->range_effective = (g_range_override != AVCOL_RANGE_UNSPECIFIED)
                         ? g_range_override : d->range;

    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(d->pix_fmt);
    d->bit_depth = pd ? pd->comp[0].depth : 8;

    extract_stream_sidedata(d, st);
    log_metadata(d);
    if (g_range_override != AVCOL_RANGE_UNSPECIFIED)
        LOG("META", "range forced to %s by --range", rng_name(g_range_override));

    d->pkt   = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (d->hw_requested)
        d->hw_frame = av_frame_alloc();
    return d->pkt && d->frame && (!d->hw_requested || d->hw_frame);
}

/* ------------------------------------------------------------------ */
/* Seeking                                                             */
/*                                                                     */
/* Elementary streams — Annex-B .h264/.h265, raw .mpeg2video and the   */
/* like — carry neither timestamps nor an index, and FFmpeg marks them */
/* AVFMT_NOTIMESTAMPS. Asking av_seek_frame() for a timestamp on one   */
/* is actively destructive: with no read_seek and no index entries it  */
/* falls through to the generic search, which rewinds to the data      */
/* offset and reads forward hunting for a DTS past the target. Every   */
/* DTS is AV_NOPTS_VALUE, so nothing ever matches — it consumes the    */
/* whole file, returns -1, and leaves the demuxer parked at EOF with   */
/* the stream unusable for the rest of the run. Byte seeking is the    */
/* one operation these formats do support, and for "back to the        */
/* start" it is exact.                                                 */
/* ------------------------------------------------------------------ */
static bool untimestamped(const Decoder *d)
{
    return d->fmt && d->fmt->iformat &&
           (d->fmt->iformat->flags & AVFMT_NOTIMESTAMPS);
}

static bool can_byte_seek(const Decoder *d)
{
    if (!d->fmt || !d->fmt->iformat) return false;
    if (d->fmt->iformat->flags & AVFMT_NO_BYTE_SEEK) return false;
    return d->fmt->pb && d->fmt->pb->seekable;
}

/* Whether the head of the stream is reachable again. Checked BEFORE
 * anything consumes frames speculatively. */
static bool can_rewind(const Decoder *d)
{
    if (!d->fmt || !d->fmt->pb || !d->fmt->pb->seekable) return false;
    return untimestamped(d) ? can_byte_seek(d) : true;
}

static bool seek_bytes(Decoder *d, int64_t pos)
{
    if (!can_byte_seek(d)) return false;
    if (av_seek_frame(d->fmt, d->stream_idx, pos,
                      AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avcodec_flush_buffers(d->cc);
    return true;
}

/* See decoder.h. The pixels are the only evidence left once the
 * container declined to say, and getting it wrong is not cosmetic:
 * decoding full-range shadows as limited pushes codes 1..15 into the
 * "black" bucket and divides the signal value of everything just above
 * by roughly ten, which is exactly where the low percentiles live. */
bool decoder_resolve_color_range(Decoder *d, int max_frames)
{
    if (!d) return false;
    if (d->range_effective != AVCOL_RANGE_UNSPECIFIED) return true;
    if (max_frames < 1) max_frames = 1;

    /* Consuming frames is only acceptable if they can be put back. */
    if (!can_rewind(d)) {
        LOG("META", "range unspecified and input not seekable — reading as limited");
        return false;
    }

    double sum = 0.0;
    int    n   = 0;
    for (int i = 0; i < max_frames && decoder_next_frame(d) > 0; i++) {
        double frac = 0.0;
        if (probe_guess_color_range(d->frame, &frac) == AVCOL_RANGE_UNSPECIFIED)
            continue;   /* pixel format the probe cannot read */
        sum += frac;
        n++;
    }

    bool rewound = decoder_seek_start(d);
    if (!n) {
        LOG("META", "range unspecified and no frame was readable — reading as limited");
        return false;
    }
    if (!rewound)
        LOG("DEC", "WARNING: could not rewind after the range probe; "
                   "the stream is left wherever the failed seek put it");

    /* Pooled over frames, not voted: a single fade-in or title card
     * carries no excursions and would otherwise outvote the content. */
    d->range_outside_frac = sum / n;
    d->range_guessed      = true;
    d->range_effective    = (d->range_outside_frac > PROBE_RANGE_FULL_FRAC)
                            ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    LOG("META", "range unspecified — %.3f%% of luma outside the limited-range "
                "window over %d frame%s, reading as %s",
        d->range_outside_frac * 100.0, n, n == 1 ? "" : "s",
        rng_name(d->range_effective));
    return true;
}

/* Consecutive AVERROR(EAGAIN)s from av_read_frame tolerated before the
 * demuxer is called stuck. One millisecond apart, so this is also the
 * wait in milliseconds. */
#define READ_EAGAIN_LIMIT 1000

int decoder_next_frame(Decoder *d)
{
    int eagain = 0;

    for (;;) {
        AVFrame *decoded = d->hw_frame ? d->hw_frame : d->frame;
        int r = avcodec_receive_frame(d->cc, decoded);
        if (r == 0) {
            if (decoded != d->frame) {
                av_frame_unref(d->frame);
                if (decoded->format == d->hw_pix_fmt) {
                    r = av_hwframe_transfer_data(d->frame, decoded, 0);
                    if (r >= 0)
                        r = av_frame_copy_props(d->frame, decoded);
                    av_frame_unref(decoded);
                    if (r < 0) {
                        char msg[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(r, msg, sizeof(msg));
                        LOG("DEC", "hardware frame download failed: %s", msg);
                        return -1;
                    }
                    if (!d->hw_format_logged) {
                        LOG("DEC", "%s output downloaded as %s", d->hw_name,
                            av_get_pix_fmt_name(d->frame->format) ?: "unknown");
                        d->hw_format_logged = true;
                    }
                } else {
                    /* get_format selected the software fallback. */
                    av_frame_move_ref(d->frame, decoded);
                }
            }
            /* Stamp the resolved range on the frame rather than
             * handing it to each consumer separately: the probe, the
             * session accumulator's format-change test and libplacebo
             * all read it from here, so this is the one place that
             * keeps them from disagreeing. */
            if (d->range_effective != AVCOL_RANGE_UNSPECIFIED)
                d->frame->color_range = d->range_effective;
            if (d->frame_index >= 0) d->frame_index++;
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) {
            char msg[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(r, msg, sizeof(msg));
            LOG("DEC", "ERROR: decode failed: %s", msg);
            return -1;
        }

        /* Need more input. */
        r = av_read_frame(d->fmt, d->pkt);
        if (r == AVERROR_EOF) {
            avcodec_send_packet(d->cc, NULL); /* drain */
            continue;
        }
        /* A demuxer with nothing ready yet has not ended. Returning -1
         * here made the caller mark the source finished, which surfaces
         * as a bare "EOF" — indistinguishable from the real thing. */
        if (r == AVERROR(EAGAIN)) {
            av_packet_unref(d->pkt);
            if (++eagain > READ_EAGAIN_LIMIT) {
                LOG("DEC", "ERROR: demuxer stalled (no packet in %d ms)",
                    READ_EAGAIN_LIMIT);
                return -1;
            }
            av_usleep(1000);
            continue;
        }
        if (r < 0) {
            /* Every other read failure — truncated file, I/O error, a
             * moov box that lied about the sample table. Silence here is
             * what made a broken stream look like a finished one. */
            char msg[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(r, msg, sizeof(msg));
            LOG("DEC", "ERROR: read failed: %s", msg);
            return -1;
        }
        eagain = 0;

        if (d->pkt->stream_index == d->stream_idx) {
            /* Any failure, EAGAIN included: output is drained above
             * before every send, so the decoder cannot be asking for a
             * read here. If it ever does, the packet this unrefs is one
             * we silently never decoded — better said out loud. */
            int s = avcodec_send_packet(d->cc, d->pkt);
            if (s < 0) {
                char msg[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(s, msg, sizeof(msg));
                LOG("DEC", "ERROR: submit failed: %s", msg);
                av_packet_unref(d->pkt);
                return -1;
            }
        }
        av_packet_unref(d->pkt);
    }
}

bool decoder_seek_start(Decoder *d)
{
    /* Seek to the very beginning, then flush the decoder so any buffered
     * frames from the previous pass don't leak into the next. */
    if (untimestamped(d)) {
        if (!seek_bytes(d, 0)) {
            LOG("DEC", "rewind failed: %s carries no timestamps and the "
                       "input cannot be byte-seeked", d->fmt->iformat->name);
            return false;
        }
        d->frame_index = 0;
        return true;
    }

    int r = av_seek_frame(d->fmt, d->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    if (r < 0) {
        /* A failed generic seek can leave the demuxer at EOF. A byte seek
         * both rewinds and repairs that, so try it before giving up. */
        if (seek_bytes(d, 0)) { d->frame_index = 0; return true; }
        LOG("DEC", "seek to start failed: %d", r);
        return false;
    }
    avcodec_flush_buffers(d->cc);
    d->frame_index = 0;
    return true;
}

bool decoder_seek_to(Decoder *d, double seconds)
{
    if (seconds < 0.0) seconds = 0.0;

    /* No timebase to seek against: position is counted in frames, and a
     * seek is "rewind if the target is behind us, then decode forward".
     * Linear in the distance moved, which is the price of a format that
     * ships no index — but it is exact, and it never leaves the demuxer
     * somewhere the next read cannot recover from. */
    if (untimestamped(d)) {
        AVRational fr = av_guess_frame_rate(d->fmt,
                                            d->fmt->streams[d->stream_idx], NULL);
        double fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        int64_t target = (fps > 0.0) ? (int64_t)(seconds * fps + 0.5) : 0;

        if (d->frame_index < 0 || target < d->frame_index) {
            if (!decoder_seek_start(d)) return false;
        }
        while (d->frame_index < target) {
            int r = decoder_next_frame(d);
            /* EOF is a legitimate landing place — the caller asked to go
             * past the end. An actual decode error is not. */
            if (r < 0) return false;
            if (r == 0) break;
        }
        return true;
    }

    /* Seek to an absolute timestamp. AVSEEK_FLAG_BACKWARD lands on a
     * keyframe at or before the target — required to start decoding
     * from a clean reference. The next decoded frame may therefore
     * come from slightly before `seconds`; that's fine for ±10s
     * stepping where the user doesn't expect frame-precise landing. */
    AVRational tb = d->fmt->streams[d->stream_idx]->time_base;
    int64_t target = (int64_t)(seconds * tb.den / tb.num);
    int r = av_seek_frame(d->fmt, d->stream_idx, target, AVSEEK_FLAG_BACKWARD);
    if (r < 0) { LOG("DEC", "seek to %.2fs failed: %d", seconds, r); return false; }
    avcodec_flush_buffers(d->cc);
    d->frame_index = -1;   /* unknown; only the untimestamped path reads it */
    return true;
}

double decoder_frame_seconds(const Decoder *d)
{
    if (!d->frame) return NAN;
    int64_t pts = d->frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) return NAN;
    AVRational tb = d->fmt->streams[d->stream_idx]->time_base;
    return (double)pts * tb.num / tb.den;
}

void decoder_close(Decoder *d)
{
    if (d->hw_frame) av_frame_free(&d->hw_frame);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->cc)    avcodec_free_context(&d->cc);
    if (d->fmt)   avformat_close_input(&d->fmt);
}
