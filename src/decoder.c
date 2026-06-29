#include "decoder.h"
#include "log.h"

#include <math.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>

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
        LOG("META", "no mastering display metadata (HDR10 will fall back to defaults)");
    }
    if (d->has_cll)
        LOG("META", "content light: MaxCLL=%d MaxFALL=%d", d->cll_max, d->cll_avg);
    else
        LOG("META", "no MaxCLL/MaxFALL");
}

/* HDR10 static metadata lives on the stream as side data. Some encoders
 * also stamp it on each AVFrame; we read from codecpar->coded_side_data
 * first, then accept per-frame upgrades silently in the render path. */
static void extract_hdr10_sidedata(Decoder *d, const AVStream *st)
{
#if LIBAVCODEC_VERSION_MAJOR >= 60
    const AVPacketSideData *sd_mdcv = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    const AVPacketSideData *sd_cll = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
#else
    /* Older ffmpeg: iterate codecpar->coded_side_data manually. */
    const AVPacketSideData *sd_mdcv = NULL, *sd_cll = NULL;
    for (int i = 0; i < st->codecpar->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &st->codecpar->coded_side_data[i];
        if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) sd_mdcv = sd;
        if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL)        sd_cll  = sd;
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
}

bool decoder_open(Decoder *d, const char *path)
{
    memset(d, 0, sizeof(*d));

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

    d->cc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->cc, st->codecpar);
    if (avcodec_open2(d->cc, codec, NULL) < 0) {
        LOG("DEC", "ERROR: cannot open decoder");
        return false;
    }

    d->width     = d->cc->width;
    d->height    = d->cc->height;
    d->primaries = d->cc->color_primaries;
    d->transfer  = d->cc->color_trc;
    d->matrix    = d->cc->colorspace;
    d->range     = d->cc->color_range;
    d->pix_fmt   = d->cc->pix_fmt;

    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(d->pix_fmt);
    d->bit_depth = pd ? pd->comp[0].depth : 8;

    extract_hdr10_sidedata(d, st);
    log_metadata(d);

    d->pkt   = av_packet_alloc();
    d->frame = av_frame_alloc();
    return true;
}

int decoder_next_frame(Decoder *d)
{
    for (;;) {
        int r = avcodec_receive_frame(d->cc, d->frame);
        if (r == 0)        return 1;
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) { LOG("DEC", "decode error %d", r); return -1; }

        /* Need more input. */
        r = av_read_frame(d->fmt, d->pkt);
        if (r == AVERROR_EOF) {
            avcodec_send_packet(d->cc, NULL); /* drain */
            continue;
        }
        if (r < 0) return -1;

        if (d->pkt->stream_index == d->stream_idx)
            avcodec_send_packet(d->cc, d->pkt);
        av_packet_unref(d->pkt);
    }
}

bool decoder_seek_start(Decoder *d)
{
    /* Seek to the very beginning, then flush the decoder so any buffered
     * frames from the previous pass don't leak into the next. */
    int r = av_seek_frame(d->fmt, d->stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    if (r < 0) { LOG("DEC", "seek to start failed: %d", r); return false; }
    avcodec_flush_buffers(d->cc);
    return true;
}

bool decoder_seek_to(Decoder *d, double seconds)
{
    /* Seek to an absolute timestamp. AVSEEK_FLAG_BACKWARD lands on a
     * keyframe at or before the target — required to start decoding
     * from a clean reference. The next decoded frame may therefore
     * come from slightly before `seconds`; that's fine for ±10s
     * stepping where the user doesn't expect frame-precise landing. */
    if (seconds < 0.0) seconds = 0.0;
    AVRational tb = d->fmt->streams[d->stream_idx]->time_base;
    int64_t target = (int64_t)(seconds * tb.den / tb.num);
    int r = av_seek_frame(d->fmt, d->stream_idx, target, AVSEEK_FLAG_BACKWARD);
    if (r < 0) { LOG("DEC", "seek to %.2fs failed: %d", seconds, r); return false; }
    avcodec_flush_buffers(d->cc);
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
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->cc)    avcodec_free_context(&d->cc);
    if (d->fmt)   avformat_close_input(&d->fmt);
}
