/* hdrplay — a minimal HDR video player built for insight.
 *
 * Pipeline:
 *   file -> libavformat (demux)
 *        -> libavcodec  (decode, preserves HDR side data)
 *        -> libplacebo  (color management, tone map, render)
 *        -> Vulkan      (GPU API; MoltenVK on macOS)
 *        -> SDL3        (window + HDR-aware swapchain colorspace)
 *        -> compositor  (CoreAnimation/Wayland/DWM signals HDR to panel)
 *
 * Run with:   ./hdrplay path/to/video.mp4 [-v]
 *
 * Every interesting step logs to stderr with a tag, so:
 *   ./hdrplay video.mp4 2>&1 | grep '^\[HDR\]'   # display state
 *   ./hdrplay video.mp4 2>&1 | grep '^\[REND\]'  # per-frame placebo decisions
 */

#include "decoder.h"
#include "renderer.h"
#include "diagnose.h"
#include "brightness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <SDL3/SDL.h>
#include <libavutil/rational.h>

/* Monotonic wall clock in seconds, for pacing playback. */
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int g_verbose = 0;

/* macOS has no built-in Vulkan driver; we need MoltenVK and an ICD
 * manifest pointing the loader at it. If the user already exported
 * VK_ICD_FILENAMES we respect it; otherwise probe a few well-known
 * spots — including the bundled copy we shipped in third_party/.
 *
 * Bundled-copy candidates are resolved relative to the executable's
 * own directory (via _NSGetExecutablePath), NOT the CWD. Earlier
 * versions used CWD-relative paths, which silently broke when the
 * user ran hdrplay from anywhere outside hdrplay/ or hdrplay/build/
 * — SDL would then fall back to whatever Vulkan loader macOS finds
 * by default, which typically doesn't expose VK_KHR_surface, and
 * window creation would fail with a cryptic extension error. */
static void ensure_moltenvk_icd(void)
{
#ifdef __APPLE__
    if (getenv("VK_ICD_FILENAMES")) {
        LOG("GPU", "VK_ICD_FILENAMES already set: %s", getenv("VK_ICD_FILENAMES"));
        return;
    }

    /* Resolve exe dir for bundled-MoltenVK lookups. */
    char exe_path[PATH_MAX] = {0};
    char exe_real[PATH_MAX] = {0};
    char exe_dir[PATH_MAX]  = {0};
    uint32_t sz = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &sz) == 0 &&
        realpath(exe_path, exe_real))
    {
        strncpy(exe_dir, exe_real, sizeof(exe_dir) - 1);
        char *slash = strrchr(exe_dir, '/');
        if (slash) *slash = '\0';
    }

    char c_build[PATH_MAX], c_repo[PATH_MAX], c_sibling[PATH_MAX];
    snprintf(c_build,   sizeof(c_build),
             "%s/../third_party/MoltenVK/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json",
             exe_dir);
    snprintf(c_repo,    sizeof(c_repo),
             "%s/third_party/MoltenVK/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json",
             exe_dir);
    snprintf(c_sibling, sizeof(c_sibling),
             "%s/MoltenVK_icd.json", exe_dir);

    const char *candidates[] = {
        c_build,    /* exe at hdrplay/build/hdrplay, ICD at hdrplay/third_party/... */
        c_repo,     /* exe + third_party as siblings (alternate layouts) */
        c_sibling,  /* manifest shipped alongside the binary */
        /* Homebrew install: `brew install molten-vk` lays the ICD
         * down at <prefix>/share/vulkan/icd.d. /opt/homebrew is
         * Apple Silicon; /usr/local is Intel. The keg-rooted path
         * is the canonical install location; the share-rooted path
         * is Homebrew's symlinked one. */
        "/opt/homebrew/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        "/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/opt/molten-vk/share/vulkan/icd.d/MoltenVK_icd.json",
        "/usr/local/share/vulkan/icd.d/MoltenVK_icd.json",
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            setenv("VK_ICD_FILENAMES", candidates[i], 0);
            LOG("GPU", "MoltenVK ICD found: %s", candidates[i]);
            return;
        }
    }
    LOG("GPU", "WARNING: no MoltenVK_icd.json found; Vulkan init will fail.");
    LOG("GPU", "         Download MoltenVK-macos.tar from KhronosGroup/MoltenVK");
    LOG("GPU", "         releases and extract to ./third_party/, or set");
    LOG("GPU", "         VK_ICD_FILENAMES manually.");
#endif
}

static void usage(void)
{
    fprintf(stderr,
        "usage: hdrplay [opts] <input>\n"
        "       hdrplay --diagnose [-d INDEX]\n"
        "       hdrplay --set-brightness FLOAT [-d INDEX]\n"
        "       hdrplay --list-displays\n"
        "\n"
        "playback options:\n"
        "  -v                    verbose per-frame logging\n"
        "  -f                    start fullscreen (recommended for true\n"
        "                        EDR — CoreAnimation caps headroom for\n"
        "                        windowed surfaces)\n"
        "  -d INDEX              place the window on display INDEX\n"
        "                        (0-based; see --list-displays)\n"
        "  --start-sdr           start in SDR-fallback rendering mode\n"
        "  --split               start in split-screen (HDR left, SDR right)\n"
        "  --split-tb            split top/bottom instead of left/right\n"
        "  --split-diag          diagonal split: HDR upper-left, SDR lower-right\n"
        "                        — best demo of what 'broken HDR' looks like\n"
        "  --loop                rewind to start on EOF instead of quitting\n"
        "  --sdr-peak NITS       SDR tone-map ceiling in nits.\n"
        "                          default = OS-tracked (~500n; matches\n"
        "                            QuickTime / macOS SDR composition)\n"
        "                          100 = strict BT.2100 spec (dim)\n"
        "                          203 = BT.2408 HDR Video reference\n"
        "                          800 = Apple Display preset (bright)\n"
        "  --sdr-saturation N    SDR-pass saturation gain. 1.0 = libplacebo\n"
        "                        default (perceptually-tuned, less vivid).\n"
        "                        Default 1.0 (hue-faithful); >1 shifts\n"
        "                        saturated reds toward orange.\n"
        "  --sdr-dr-stops N      SDR-pane dynamic-range cap in stops.\n"
        "                        Sets sdr_min = sdr_peak / 2^N so the\n"
        "                        SDR pane shows only what an SDR display\n"
        "                        could actually deliver. Default 12.0\n"
        "                        (matches 0.1-nit BT.1886 reference\n"
        "                        black at the 500-nit modern SDR peak;\n"
        "                        use 10.0 for strict 100-nit BT.1886).\n"
        "                        Larger values reveal more shadow detail\n"
        "                        than SDR can really show.\n"
        "  --sdr-gamut-map MODE  How SDR pass handles BT.2020 colors that\n"
        "                        don't fit in BT.709 (a real second axis of\n"
        "                        HDR/SDR difference, alongside peak nits):\n"
        "                          perceptual (default) = BT.2407 rolloff,\n"
        "                            matches HDR-display internal mapping\n"
        "                          relative = relative colorimetric, matches\n"
        "                            OS color-management BT.2020→BT.709\n"
        "                          clip = hard clip (oversaturated edges)\n"
        "                          off = no mapping (libplacebo's default;\n"
        "                            may pass wide-gamut colors through and\n"
        "                            mask the gamut-narrowing advantage HDR\n"
        "                            has over SDR)\n"
        "  keys at runtime:      F=fullscreen  H=HDR  S=SDR  P=split\n"
        "                        O=toggle split orientation  SPACE=pause\n"
        "                        L=toggle loop  R=restart  Q/Esc=quit\n"
        "                        M=toggle luminance probe (mouse → nits)\n"
        "\n"
        "control / inspection:\n"
        "  --list-displays       enumerate displays + HDR status, then exit\n"
        "  --diagnose            run HDR sanity checks; exit code = # FAILs.\n"
        "                        Pair with -d to check one display only.\n"
        "  --set-brightness F    set brightness 0.0-1.0 on the chosen\n"
        "                        display (built-in: IOKit / `brightness`;\n"
        "                        external DDC/CI: m1ddc). XDR is not\n"
        "                        settable programmatically.\n");
}

int main(int argc, char **argv)
{
    bool fullscreen = false;
    bool list_displays_only = false;
    bool diagnose_only = false;
    bool has_brightness = false;
    float brightness_val = -1.0f;
    int  display_index = -1;
    int  start_mode = HDRPLAY_MODE_HDR;
    int  start_orient = HDRPLAY_SPLIT_LR;
    bool loop_at_eof = false;
    float sdr_peak_override = 0.0f;   /* 0 = OS-tracked default */
    float sdr_saturation    = 1.0f;   /* 1.0 = libplacebo native; >1 shifts saturated reds toward orange */
    const struct pl_gamut_map_function *sdr_gamut_map = &pl_gamut_map_perceptual;
    float sdr_dr_stops_cap = 12.0f;   /* matches 0.1-nit BT.1886 black at 500-nit modern SDR peak */
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-f")) fullscreen = true;
        else if (!strcmp(argv[i], "--list-displays")) list_displays_only = true;
        else if (!strcmp(argv[i], "--diagnose")) diagnose_only = true;
        else if (!strcmp(argv[i], "--start-sdr")) start_mode = HDRPLAY_MODE_SDR;
        else if (!strcmp(argv[i], "--split"))     start_mode = HDRPLAY_MODE_SPLIT;
        else if (!strcmp(argv[i], "--split-tb"))   { start_mode = HDRPLAY_MODE_SPLIT; start_orient = HDRPLAY_SPLIT_TB; }
        else if (!strcmp(argv[i], "--split-lr"))   { start_mode = HDRPLAY_MODE_SPLIT; start_orient = HDRPLAY_SPLIT_LR; }
        else if (!strcmp(argv[i], "--split-diag")) { start_mode = HDRPLAY_MODE_SPLIT; start_orient = HDRPLAY_SPLIT_DIAG; }
        else if (!strcmp(argv[i], "--loop"))     loop_at_eof = true;
        else if (!strcmp(argv[i], "--sdr-peak") && i+1 < argc)
            sdr_peak_override = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sdr-saturation") && i+1 < argc)
            sdr_saturation = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sdr-dr-stops") && i+1 < argc)
            sdr_dr_stops_cap = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sdr-gamut-map") && i+1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "perceptual")) sdr_gamut_map = &pl_gamut_map_perceptual;
            else if (!strcmp(m, "relative"))   sdr_gamut_map = &pl_gamut_map_relative;
            else if (!strcmp(m, "clip"))       sdr_gamut_map = &pl_gamut_map_clip;
            else if (!strcmp(m, "off"))        sdr_gamut_map = NULL;
            else { fprintf(stderr, "unknown --sdr-gamut-map mode: %s\n", m); usage(); return 2; }
        }
        else if (!strcmp(argv[i], "--set-brightness") && i+1 < argc) {
            has_brightness = true; brightness_val = (float)atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "-d") && i+1 < argc) display_index = atoi(argv[++i]);
        else if (argv[i][0] == '-')      { usage(); return 2; }
        else if (!path)                  path = argv[i];
        else                             { usage(); return 2; }
    }

    ensure_moltenvk_icd();
    /* Silence MoltenVK's 150-line extension dump unless user asks for it.
     * 1=error only, 3=info, 4=debug. Insight comes from our own [REND]/
     * [META]/[HDR] logs, not from MVK's. */
    setenv("MVK_CONFIG_LOG_LEVEL", g_verbose ? "3" : "1", 0);

    if (list_displays_only) {
        fprintf(stderr, "displays:\n");
        renderer_list_displays();
        return 0;
    }
    if (has_brightness) {
        SDL_Init(SDL_INIT_VIDEO);
        return brightness_set(display_index < 0 ? 0 : display_index, brightness_val);
    }
    if (diagnose_only) {
        return diagnose_run(display_index);
    }
    if (!path) { usage(); return 2; }

    /* Always log displays at startup — picking the right one is a
     * common first source of "why isn't this HDR?" confusion. */
    fprintf(stderr, "[GPU] displays:\n");
    renderer_list_displays();

    Decoder dec;
    if (!decoder_open(&dec, path)) return 1;

    Renderer rend;
    if (!renderer_init(&rend, dec.width, dec.height, "hdrplay", display_index)) {
        decoder_close(&dec);
        return 1;
    }

    if (fullscreen)
        SDL_SetWindowFullscreen(rend.window, true);

    rend.mode = start_mode;
    rend.split_orient = start_orient;
    rend.loop_enabled = loop_at_eof;
    rend.sdr_peak_override = sdr_peak_override;
    rend.sdr_saturation    = sdr_saturation;
    rend.sdr_gamut_map     = sdr_gamut_map;
    rend.sdr_dr_stops_cap  = sdr_dr_stops_cap;
    const char *orient_name =
        start_orient == HDRPLAY_SPLIT_TB   ? " (top/bottom)" :
        start_orient == HDRPLAY_SPLIT_DIAG ? " (diagonal)"   :
                                             " (left/right)";
    LOG("REND", "starting in mode: %s%s",
        start_mode == HDRPLAY_MODE_HDR   ? "HDR" :
        start_mode == HDRPLAY_MODE_SDR   ? "SDR" : "SPLIT",
        start_mode == HDRPLAY_MODE_SPLIT ? orient_name : "");

    /* Main loop: blocking decode/render. No audio, no A/V sync, but we
     * DO pace presentation against the file's PTS so playback runs at
     * native speed instead of as-fast-as-the-GPU-can-go.
     *
     * Pacing baseline: on the first frame after start / seek / resume,
     * we record (wall_clock_now, frame_pts) as a baseline. For every
     * subsequent frame we compute target_wall = baseline_wall +
     * (frame_pts - baseline_pts) * stream_time_base, then sleep until
     * target_wall before submitting. If we're already late (decoder or
     * GPU couldn't keep up — common for 4K HDR on integrated GPUs), we
     * skip the sleep and log a [DEC] behind=Nms line so it's visible.
     *
     * Pause keeps rendering the last decoded frame so HDR/SDR/split
     * toggles remain interactive even while frozen. Resume re-baselines
     * the clock so the pause itself doesn't show up as "behind". */
    bool quit   = false;
    bool paused = false;
    bool have_frame = false;
    AVRational stream_tb = dec.fmt->streams[dec.stream_idx]->time_base;
    double  base_wall   = 0.0;
    int64_t base_pts    = AV_NOPTS_VALUE;   /* AV_NOPTS_VALUE = rebase next frame */
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q) quit = true;
                if (e.key.key == SDLK_F) {
                    bool now_fs = SDL_GetWindowFlags(rend.window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(rend.window, !now_fs);
                }
                if (e.key.key == SDLK_H) { rend.mode = HDRPLAY_MODE_HDR;   LOG("REND", "mode -> HDR"); }
                if (e.key.key == SDLK_S) { rend.mode = HDRPLAY_MODE_SDR;   LOG("REND", "mode -> SDR"); }
                if (e.key.key == SDLK_P) { rend.mode = HDRPLAY_MODE_SPLIT; LOG("REND", "mode -> SPLIT"); }
                if (e.key.key == SDLK_O) {
                    /* Cycle LR → TB → DIAG → LR. */
                    rend.split_orient =
                        rend.split_orient == HDRPLAY_SPLIT_LR  ? HDRPLAY_SPLIT_TB :
                        rend.split_orient == HDRPLAY_SPLIT_TB  ? HDRPLAY_SPLIT_DIAG :
                                                                 HDRPLAY_SPLIT_LR;
                    const char *name =
                        rend.split_orient == HDRPLAY_SPLIT_TB   ? "top/bottom" :
                        rend.split_orient == HDRPLAY_SPLIT_DIAG ? "diagonal" :
                                                                  "left/right";
                    LOG("REND", "split orientation -> %s", name);
                }
                if (e.key.key == SDLK_SPACE) {
                    paused = !paused;
                    rend.paused = paused;
                    /* Resume → rebase clock so the pause duration
                     * doesn't register as decoder-behind. */
                    if (!paused) base_pts = AV_NOPTS_VALUE;
                    LOG("DEC", "%s", paused ? "paused" : "resumed");
                }
                if (e.key.key == SDLK_L) {
                    loop_at_eof = !loop_at_eof;
                    rend.loop_enabled = loop_at_eof;
                    LOG("DEC", "loop %s", loop_at_eof ? "ON" : "OFF");
                }
                if (e.key.key == SDLK_R) {
                    if (decoder_seek_start(&dec)) {
                        base_pts = AV_NOPTS_VALUE;   /* rebase clock */
                        LOG("DEC", "restarted from beginning");
                    }
                }
                if (e.key.key == SDLK_M) {
                    rend.probe_active = !rend.probe_active;
                    LOG("REND", "luminance probe %s", rend.probe_active ? "ON" : "OFF");
                }
            }
            /* Continuously update probe coords as the mouse moves. We
             * also re-snapshot the window size so probe maps correctly
             * after the user resizes. */
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                int w = 0, h = 0;
                SDL_GetWindowSize(rend.window, &w, &h);
                rend.probe_x      = (int)e.motion.x;
                rend.probe_y      = (int)e.motion.y;
                rend.probe_win_w  = w;
                rend.probe_win_h  = h;
            }
            if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
                e.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) {
                LOG("HDR", "window moved or HDR state changed — re-querying");
                renderer_update_display_state(&rend);
            }
        }

        if (!paused) {
            int r = decoder_next_frame(&dec);
            if (r == 0) {
                LOG("DEC", "EOF");
                if (loop_at_eof && decoder_seek_start(&dec)) {
                    base_pts = AV_NOPTS_VALUE;   /* rebase clock on loop */
                    continue;
                }
                quit = true; break;
            }
            if (r < 0)  { LOG("DEC", "decode error, exiting"); break; }
            have_frame = true;

            /* Pace presentation against the file's PTS so playback runs
             * at native speed. Frames without PTS (rare) just render
             * immediately; the FIFO swapchain still caps to refresh
             * rate as a fallback. */
            int64_t pts = dec.frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = dec.frame->pts;
            if (pts != AV_NOPTS_VALUE) {
                double tb = av_q2d(stream_tb);
                if (base_pts == AV_NOPTS_VALUE) {
                    base_pts  = pts;
                    base_wall = now_seconds();
                } else {
                    double frame_t   = (double)(pts - base_pts) * tb;
                    double target    = base_wall + frame_t;
                    double delay     = target - now_seconds();
                    if (delay > 0.0) {
                        /* Sleep up to ~1 second; never longer (guards
                         * against PTS discontinuities). */
                        if (delay > 1.0) delay = 1.0;
                        struct timespec ts = {
                            .tv_sec  = (time_t)delay,
                            .tv_nsec = (long)((delay - (double)(time_t)delay) * 1e9),
                        };
                        nanosleep(&ts, NULL);
                    } else if (delay < -0.050) {
                        /* More than 50ms behind. Don't drop (insight
                         * tool) but make it visible. */
                        LOGV("DEC", "behind=%.0fms (pts=%.3fs)",
                             -delay * 1000.0, frame_t);
                    }
                }
            }
        }

        if (have_frame) {
            renderer_render_avframe(&rend, dec.frame);
        } else {
            /* Paused before any frame decoded — nothing to draw, idle. */
            SDL_Delay(16);
        }
    }

    renderer_close(&rend);
    decoder_close(&dec);
    return 0;
}
