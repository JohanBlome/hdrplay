#ifndef HDRPLAY_SOURCE_H
#define HDRPLAY_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "decoder.h"
#include "probe.h"
#include "stats.h"

/* ------------------------------------------------------------------ */
/* A single playable input.                                            */
/*                                                                     */
/* Sources do not own a clock. A master clock in main.c owns time, and  */
/* each source independently advances to whatever frame is in effect at */
/* that instant. That is what makes two files with different frame      */
/* rates line up: 24fps and 30fps land on different frame INDICES at    */
/* the same moment, which is the correct reading of "synchronized".     */
/* ------------------------------------------------------------------ */

/* Ring of recently shown frames, so stepping backwards does not have to
 * seek. Video decoding is one-directional — frame N-1 is only reachable
 * by seeking to the preceding keyframe and decoding forward again — so
 * without this every backward nudge costs a full GOP.
 *
 * `back` is a cursor: 0 means showing the newest frame, 1 the one
 * before it, and so on. Stepping forward walks the cursor toward 0 and
 * only decodes once it gets there. Frames are held by reference
 * (av_frame_ref), not copied. */
typedef struct {
    struct AVFrame **buf;
    int cap;      /* 0 disables the ring entirely            */
    int len;      /* frames held; buf[len-1] is the newest    */
    int back;     /* steps back from newest; 0 = live         */
} FrameRing;

void            ring_init(FrameRing *r, int cap);
void            ring_free(FrameRing *r);
/* Takes ownership of one reference to `f`. Drops the oldest when full,
 * and resets the cursor to live. */
void            ring_push(FrameRing *r, struct AVFrame *f);
struct AVFrame *ring_current(const FrameRing *r);
bool            ring_step_back(FrameRing *r);   /* false = past the end */
bool            ring_step_fwd(FrameRing *r);    /* false = already live */
void            ring_clear(FrameRing *r);

/* ------------------------------------------------------------------ */
typedef struct Source {
    Decoder  dec;
    char     label[64];        /* basename, for the pane badge         */
    double   tb_sec;           /* stream timebase in seconds           */
    double   duration_sec;     /* <= 0 when unknown                    */

    struct AVFrame *shown;     /* frame currently on screen (owned)    */
    struct AVFrame *pending;   /* decoded but not yet due (owned)      */
    bool     eof;
    int      frame_no;         /* index of `shown`, for the HUD        */

    FrameRing ring;

    SessionStats session;
    FrameStats   frame_stats;
    bool         frame_stats_valid;
} Source;

bool source_open(Source *s, const char *path, int ring_cap);
void source_close(Source *s);

/* Present-time of the frame currently shown, or NAN. */
double source_shown_sec(const Source *s);
/* Present-time of the next undisplayed frame, decoding one if needed.
 * NAN at EOF. */
double source_peek_next_sec(Source *s);

/* Advance until the frame in effect at `t` seconds is shown — i.e. the
 * last frame whose PTS is <= t. Returns true if the shown frame
 * changed. At EOF the last frame is held rather than going black. */
bool source_advance_to(Source *s, double t);

/* Show the next frame and return its time, or NAN if none. Walks the
 * ring cursor forward first when stepping back has left it behind. */
double source_step_forward(Source *s);

/* Show the previous frame and return its time, or NAN if unavailable.
 * Uses the ring when it can; otherwise seeks to the preceding keyframe
 * and decodes forward, refilling the ring on the way so repeated
 * back-steps stay fast. */
double source_step_back(Source *s);

/* Drop decoded state after a seek. */
void source_flush(Source *s);

#endif
