#ifndef HDRPLAY_LOG_H
#define HDRPLAY_LOG_H

#include <stdio.h>
#include <time.h>

/* Insight-first logging. Every line is greppable by tag so you can do
 *   ./hdrplay foo.mp4 2>&1 | grep '^\[HDR\]'
 * to isolate one concern at a time.
 *
 * Tags used in this codebase:
 *   [DEC]   demux/decode events, per-stream metadata
 *   [META]  per-frame HDR metadata (HDR10 MaxCLL/MaxFALL, mastering display)
 *   [GPU]   Vulkan / libplacebo setup
 *   [SWAP]  swapchain colorspace and HDR signaling
 *   [HDR]   display HDR state changes (headroom, SDR white level)
 *   [REND]  per-frame render decisions from libplacebo
 *   [HUD]   on-screen overlay updates
 */

#define LOG(tag, fmt, ...) \
    fprintf(stderr, "[" tag "] " fmt "\n", ##__VA_ARGS__)

#define LOGV(tag, fmt, ...) do { if (g_verbose) LOG(tag, fmt, ##__VA_ARGS__); } while (0)

extern int g_verbose;

#endif
