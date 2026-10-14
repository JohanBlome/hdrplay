#ifndef HDRPLAY_VIDEO_TIMELINE_H
#define HDRPLAY_VIDEO_TIMELINE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <libavutil/rational.h>
typedef struct {
    int64_t pts, duration;
    bool key;
} VideoSample;
/* 1: MOV/MP4 sample tables, 0: another container, -1: invalid/unsupported tables.
 * Reads box headers and timing tables only; never reads mdat payloads. */
int video_timeline_read(const char *path, int track_id, AVRational stream_tb,
                         VideoSample **samples, int *count, char *error, size_t size);
#endif
