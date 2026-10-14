#ifndef HDRPLAY_MEDIA_TIME_H
#define HDRPLAY_MEDIA_TIME_H
#include <libavformat/avformat.h>
#include <math.h>
/* A shared origin preserves offsets between streams within an input. */
static inline double media_time_origin(const AVFormatContext *fmt)
{
    if (fmt->start_time != AV_NOPTS_VALUE)
        return (double)fmt->start_time / AV_TIME_BASE;
    double first = INFINITY;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVStream *st = fmt->streams[i];
        if (st->start_time != AV_NOPTS_VALUE &&
            (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
             st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
            first = fmin(first, st->start_time * av_q2d(st->time_base));
    }
    return isfinite(first) ? first : 0;
}
#endif
