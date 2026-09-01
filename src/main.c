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
#include "analyze.h"
#include "brightness.h"
#include "probe.h"
#include "stats.h"
#include "source.h"
#include "checks.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
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

/* Persistent settings live in one place: $XDG_CONFIG_HOME/hdrplay/config,
 * falling back to ~/.config/hdrplay/config. Not a dotfile in $HOME, not
 * next to the binary, not CWD-relative. Format is `key = value`, one per
 * line; blank lines and `#` comments ignored; whitespace around key and
 * value trimmed.
 *
 * Only one key is recognized today (vulkan_icd) — this is deliberately
 * the seam for playback settings when we grow them.
 *
 * Returns 1 and fills `out` if the key is present and non-empty. */
static int config_get(const char *key, char *out, size_t n)
{
    char path[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/hdrplay/config", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        snprintf(path, sizeof(path), "%s/.config/hdrplay/config", home);
    }

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int found = 0;
    char line[PATH_MAX + 128];
    while (!found && fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        /* Trim both halves in place. */
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && strchr(" \t\r\n", e[-1]); )
            *--e = '\0';
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v); e > v && strchr(" \t\r\n", e[-1]); )
            *--e = '\0';

        if (strcmp(k, key) != 0 || *v == '\0') continue;

        /* Allow a leading ~ so the file can stay portable. */
        const char *home = getenv("HOME");
        if (v[0] == '~' && v[1] == '/' && home)
            snprintf(out, n, "%s%s", home, v + 1);
        else
            snprintf(out, n, "%s", v);
        found = 1;
    }
    fclose(f);
    return found;
}

/* macOS has no built-in Vulkan driver; we need MoltenVK and an ICD
 * manifest pointing the loader at it. If the user already pointed the
 * loader at a driver we respect that; otherwise probe a few well-known
 * spots — including the bundled copy we shipped in third_party/.
 *
 * Bundled-copy candidates are resolved relative to the executable's
 * own directory (via _NSGetExecutablePath), NOT the CWD. Earlier
 * versions used CWD-relative paths, which silently broke when the
 * user ran hdrplay from anywhere outside hdrplay/ or hdrplay/build/
 * — SDL would then fall back to whatever Vulkan loader macOS finds
 * by default, which typically doesn't expose VK_KHR_surface, and
 * window creation would fail with a cryptic extension error.
 *
 * Exe-relative isn't enough either: copying the binary to ~/bin
 * without third_party/ reproduces exactly that failure. So we also
 * carry HDRPLAY_SOURCE_ICD, the absolute path to this build's source
 * tree, baked in by CMake. A binary built from this checkout works
 * wherever you put it, as long as the checkout is still there.
 *
 * Precedence, highest first: VK_DRIVER_FILES / VK_ICD_FILENAMES (a
 * one-off override), then `vulkan_icd` in ~/.config/hdrplay/config (a
 * durable one), then the probe list. */
static void ensure_moltenvk_icd(void)
{
#ifdef __APPLE__
    /* VK_DRIVER_FILES is the loader's current name for this;
     * VK_ICD_FILENAMES is the deprecated alias it still honors. Either
     * one being set means the user has an opinion — don't override. */
    const char *preset = getenv("VK_DRIVER_FILES");
    if (!preset) preset = getenv("VK_ICD_FILENAMES");
    if (preset) {
        LOG("GPU", "Vulkan driver files already set: %s", preset);
        return;
    }

    /* A durable override, for people who keep MoltenVK somewhere of
     * their own choosing and don't want to export env vars forever. */
    char c_config[PATH_MAX] = {0};
    if (config_get("vulkan_icd", c_config, sizeof(c_config)) &&
        access(c_config, R_OK) != 0)
    {
        LOG("GPU", "WARNING: config vulkan_icd is unreadable: %s", c_config);
        c_config[0] = '\0';
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

    /* LunarG SDK, if the user sourced setup-env.sh. */
    char c_sdk[PATH_MAX] = {0};
    const char *sdk = getenv("VULKAN_SDK");
    if (sdk)
        snprintf(c_sdk, sizeof(c_sdk),
                 "%s/share/vulkan/icd.d/MoltenVK_icd.json", sdk);

    const char *candidates[] = {
        c_config,   /* vulkan_icd in ~/.config/hdrplay/config */
        c_sibling,  /* manifest shipped alongside the binary (cmake --install) */
        c_build,    /* exe at hdrplay/build/hdrplay, ICD at hdrplay/third_party/... */
        c_repo,     /* exe + third_party as siblings (alternate layouts) */
#ifdef HDRPLAY_SOURCE_ICD
        HDRPLAY_SOURCE_ICD,  /* this build's source tree, baked in at compile time */
#endif
        c_sdk,      /* empty string if VULKAN_SDK unset; access() just fails */
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
        if (candidates[i][0] == '\0') continue;
        if (access(candidates[i], R_OK) != 0) continue;
        /* Set both spellings: older loaders only read VK_ICD_FILENAMES,
         * newer ones prefer VK_DRIVER_FILES and warn on the alias. */
        setenv("VK_DRIVER_FILES",   candidates[i], 0);
        setenv("VK_ICD_FILENAMES",  candidates[i], 0);
        LOG("GPU", "MoltenVK ICD found: %s", candidates[i]);
        return;
    }

    LOG("GPU", "WARNING: no MoltenVK_icd.json found; Vulkan init will fail.");
    for (int i = 0; candidates[i]; i++)
        if (candidates[i][0]) LOG("GPU", "         tried: %s", candidates[i]);
    LOG("GPU", "         Fix: extract MoltenVK-macos.tar (KhronosGroup/MoltenVK");
    LOG("GPU", "         releases) into <repo>/third_party/, then reinstall with");
    LOG("GPU", "         `cmake --install build --prefix ~`. Or set VK_DRIVER_FILES.");
#endif
}

static void usage(void)
{
    fprintf(stderr,
        "usage: hdrplay [opts] <input>\n"
        "       hdrplay [opts] <input-a> <input-b>   # compare\n"
        "       hdrplay --diagnose [-d INDEX]\n"
        "       hdrplay --set-brightness FLOAT [-d INDEX]\n"
        "       hdrplay --list-displays\n"
        "       hdrplay --analyze [--stride N] [--json] <input>\n"
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
        "  --rotate [N:]DEG      rotate an input DEG degrees clockwise\n"
        "                        before display. DEG is 0, 90, 180 or 270.\n"
        "                        Bare `--rotate 90` applies to every input;\n"
        "                        `--rotate 1:90` applies to the second file\n"
        "                        only. Inputs are numbered from 0, as in\n"
        "                        ffmpeg and as with -d (note the 1/2 solo\n"
        "                        KEYS are 1-based). Repeatable. Container\n"
        "                        rotation metadata is NOT read — this is the\n"
        "                        only source of rotation. T rotates the\n"
        "                        focused pane live.\n"
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
        "                        I=show/hide top-left status HUD\n"
        "                        A=show/hide accumulated stats panel\n"
        "                        shift-A=reset accumulated stats\n"
        "                        ←/→=seek -10s/+10s\n"
        "                        . / , =step one frame fwd/back\n"
        "                          (pauses; uses the frame ring,\n"
        "                           falls back to seek beyond it)\n"
        "                        Z=toggle 1:1 zoom   +/-=zoom steps\n"
        "                        drag or shift-arrows=pan\n"
        "                        T=rotate focused pane 90° clockwise\n"
        "                        two files: 0=compare  1/2=solo\n"
        "                                   X=swap sides\n"
        "\n"
        "control / inspection:\n"
        "  --list-displays       enumerate displays + HDR status, then exit\n"
        "  --diagnose            run HDR sanity checks; exit code = # FAILs.\n"
        "                        Pair with -d to check one display only.\n"
        "  --set-brightness F    set brightness 0.0-1.0 on the chosen\n"
        "                        display (built-in: IOKit / `brightness`;\n"
        "                        external DDC/CI: m1ddc). XDR is not\n"
        "                        settable programmatically.\n"
        "\n"
        "content analysis:\n"
        "  --analyze <input>     scan every frame headlessly (no window,\n"
        "                        no GPU) and report content checks.\n"
        "                        Exit code = # FAILs, so a batch loop\n"
        "                        works directly:\n"
        "                          for f in *.mov; do \\\n"
        "                            hdrplay --analyze \"$f\" || echo \"$f suspect\"\n"
        "                          done\n"
        "                        Exit codes >= 64 are tool errors\n"
        "                        (unreadable file / format), not verdicts.\n"
        "  --stride N            sample every Nth pixel in both axes.\n"
        "                        Default 1 = exact. Only 1 makes the\n"
        "                        MaxCLL comparison a true maximum.\n"
        "  --hlg-peak NITS       nominal display peak assumed when\n"
        "                        converting HLG scene light to display\n"
        "                        light. Default: the file's mastering\n"
        "                        display max, else 1000 (BT.2100 ref).\n"
        "                        HLG carries no absolute luminance, so\n"
        "                        every HLG nit figure rests on this.\n"
        "  --json                machine-readable summary on stdout\n"
        "                        (checks stay on stderr, so | jq works)\n"
        "  --stats-file PATH     NDJSON per-frame series + session\n"
        "                        histograms, for plotting in vca.py\n"
        "\n"
        "comparing two files:\n"
        "  hdrplay a.mov b.mov   play both synchronized, side by\n"
        "                        side in one window. A PTS master\n"
        "                        clock keeps them on the same\n"
        "                        INSTANT, so files at different\n"
        "                        frame rates line up by time rather\n"
        "                        than by frame index.\n"
        "                        The split becomes the LAYOUT, so\n"
        "                        H/S apply to both panes; press\n"
        "                        1 or 2 to solo a file and get the\n"
        "                        HDR-vs-SDR split back.\n"
        "  --step-buffer N       frames retained per file for\n"
        "                        instant step-back. Default 8.\n"
        "                        ~25MB/frame at 4K 10-bit, ~6MB at\n"
        "                        1080p. 0 disables it and always\n"
        "                        seeks (slower, no memory cost).\n");
}

/* Parse a --rotate operand into `out[2]`, degrees clockwise.
 *
 *   "90"    -> both inputs
 *   "1:90"  -> the second input only
 *
 * Inputs are numbered from 0, following ffmpeg's stream specifiers and
 * matching -d, which is already 0-based. Note this does NOT line up with
 * the 1/2 solo KEYS, which are 1-based; the CLI's own consistency was
 * judged to matter more than agreement with the keyboard.
 *
 * Returns false on a malformed index, a non-multiple of 90, or anything
 * outside 0-270. Rejecting rather than rounding is deliberate: a silently
 * ignored "--rotate 45" would look like rotation is broken. */
static bool parse_rotate(const char *arg, int out[2])
{
    int  which = -1;                 /* -1 = every input */
    const char *deg = arg;
    const char *colon = strchr(arg, ':');

    if (colon) {
        if (colon != arg + 1 || (arg[0] != '0' && arg[0] != '1')) {
            fprintf(stderr, "--rotate: input index must be 0 or 1, got \"%.*s\"\n",
                    (int)(colon - arg), arg);
            return false;
        }
        which = arg[0] - '0';
        deg   = colon + 1;
    }

    char *end = NULL;
    long d = strtol(deg, &end, 10);
    if (end == deg || *end != '\0' || d < 0 || d > 270 || d % 90 != 0) {
        fprintf(stderr, "--rotate: degrees must be 0, 90, 180 or 270, got \"%s\"\n",
                deg);
        return false;
    }

    if (which < 0) out[0] = out[1] = (int)d;
    else           out[which] = (int)d;
    return true;
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
    bool split_explicit = false;   /* user picked an orientation */
    bool loop_at_eof = false;
    float sdr_peak_override = 0.0f;   /* 0 = OS-tracked default */
    float sdr_saturation    = 1.0f;   /* 1.0 = libplacebo native; >1 shifts saturated reds toward orange */
    const struct pl_gamut_map_function *sdr_gamut_map = &pl_gamut_map_perceptual;
    float sdr_dr_stops_cap = 12.0f;   /* matches 0.1-nit BT.1886 black at 500-nit modern SDR peak */
    bool  analyze_only = false;
    bool  analyze_json = false;
    int   analyze_stride = 1;         /* exact by default; offline, so affordable */
    double hlg_peak = 0.0;            /* 0 = take mastering display, else 1000 */
    const char *stats_path = NULL;
    const char *paths[2] = { NULL, NULL };
    int   n_paths = 0;
    int   step_buffer = 8;   /* frames retained for step-back */
    int   rotation[2] = { 0, 0 };   /* degrees CW, per input */
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-f")) fullscreen = true;
        else if (!strcmp(argv[i], "--list-displays")) list_displays_only = true;
        else if (!strcmp(argv[i], "--diagnose")) diagnose_only = true;
        else if (!strcmp(argv[i], "--analyze"))  analyze_only = true;
        else if (!strcmp(argv[i], "--json"))     analyze_json = true;
        else if (!strcmp(argv[i], "--stride") && i+1 < argc)
            analyze_stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hlg-peak") && i+1 < argc)
            hlg_peak = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stats-file") && i+1 < argc)
            stats_path = argv[++i];
        else if (!strcmp(argv[i], "--step-buffer") && i+1 < argc)
            step_buffer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--start-sdr")) start_mode = HDRPLAY_MODE_SDR;
        else if (!strcmp(argv[i], "--split"))     start_mode = HDRPLAY_MODE_SPLIT;
        else if (!strcmp(argv[i], "--split-tb"))   { start_mode = HDRPLAY_MODE_SPLIT; start_orient = HDRPLAY_SPLIT_TB; split_explicit = true; }
        else if (!strcmp(argv[i], "--split-lr"))   { start_mode = HDRPLAY_MODE_SPLIT; start_orient = HDRPLAY_SPLIT_LR; split_explicit = true; }
        else if (!strcmp(argv[i], "--split-diag")) { start_mode = HDRPLAY_MODE_SPLIT; start_orient = HDRPLAY_SPLIT_DIAG; split_explicit = true; }
        else if (!strcmp(argv[i], "--loop"))     loop_at_eof = true;
        else if (!strcmp(argv[i], "--rotate") && i+1 < argc) {
            if (!parse_rotate(argv[++i], rotation)) return 2;
        }
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
        else if (n_paths < 2)            paths[n_paths++] = argv[i];
        else {
            /* Two panes, so a third file has nowhere to go. */
            fprintf(stderr, "at most two inputs (the split is the layout)\n");
            return 2;
        }
    }

    if (hlg_peak > 0.0) probe_set_hlg_peak_override(hlg_peak);

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
    /* Must return before the display enumeration below, which pulls in
     * SDL_Init(SDL_INIT_VIDEO). --analyze is a pure decode loop and has
     * to stay usable headless and over SSH. */
    if (analyze_only) {
        if (!n_paths) { usage(); return 2; }
        if (analyze_stride < 1) analyze_stride = 1;
        /* Each file in turn; the exit code is the summed FAIL
         * count so the batch loop keeps working. */
        int rc = 0;
        for (int i = 0; i < n_paths; i++) {
            int one = analyze_run(paths[i], analyze_stride, hlg_peak,
                                  analyze_json, i == 0 ? stats_path : NULL);
            if (one >= HDRPLAY_EXIT_TOOL_ERROR) return one;
            rc += one;
        }
        return rc;
    }
    if (!n_paths) { usage(); return 2; }

    /* Always log displays at startup — picking the right one is a
     * common first source of "why isn't this HDR?" confusion. */
    fprintf(stderr, "[GPU] displays:\n");
    renderer_list_displays();
    /* Open every input. Two is the ceiling: the split is the layout, so
     * a third pane has nowhere to go. */
    Source sources[2];
    int n_sources = 0;
    for (int i = 0; i < n_paths && i < 2; i++) {
        if (!source_open(&sources[n_sources], paths[i], step_buffer)) {
            for (int j = 0; j < n_sources; j++) source_close(&sources[j]);
            return 1;
        }
        n_sources++;
    }

    Renderer rend;
    if (!renderer_init(&rend, sources[0].dec.width, sources[0].dec.height,
                       "hdrplay", display_index)) {
        for (int i = 0; i < n_sources; i++) source_close(&sources[i]);
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
    rend.n_sources         = n_sources;
    rend.solo              = -1;
    rend.swapped           = false;
    rend.zoom              = 0.0f;      /* fit */
    rend.pan_x = rend.pan_y = 0.5f;
    rend.rotation[0]       = rotation[0];
    rend.rotation[1]       = rotation[1];
    rend.current_frame_no  = -1;

    for (int i = 0; i < n_sources; i++)
        if (rotation[i])
            LOG("REND", "%s rotated %d° CW", sources[i].label, rotation[i]);

    /* Two files means the split is spent on CONTENT, so H/S apply to
     * both panes and the mode starts as a plain HDR comparison rather
     * than an HDR-vs-SDR one. Solo (1/2) gets the old behaviour back. */
    if (n_sources > 1 && start_mode == HDRPLAY_MODE_HDR)
        rend.mode = HDRPLAY_MODE_HDR;
    if (n_sources > 1) {
        /* Portrait content in a left/right split gives each pane half the
         * width and all the height, so both are letterboxed into slivers.
         * Top/bottom keeps the aspect usable. Only a DEFAULT — O still
         * cycles, and an explicit --split-lr is respected.
         *
         * Measured AFTER rotation: --rotate turns a landscape-stored file
         * into portrait on screen, and it is the on-screen shape that
         * decides which split reads better. */
        int disp_w, disp_h;
        layout_rotated_dims(rotation[0], sources[0].dec.width,
                            sources[0].dec.height, &disp_w, &disp_h);
        if (!split_explicit && disp_h > disp_w) {
            rend.split_orient = HDRPLAY_SPLIT_TB;
            LOG("REND", "portrait source — defaulting to top/bottom split");
        }
        LOG("REND", "comparing %s | %s", sources[0].label, sources[1].label);
    }

    const char *orient_name =
        start_orient == HDRPLAY_SPLIT_TB   ? " (top/bottom)" :
        start_orient == HDRPLAY_SPLIT_DIAG ? " (diagonal)"   :
                                             " (left/right)";
    LOG("REND", "starting in mode: %s%s",
        rend.mode == HDRPLAY_MODE_HDR   ? "HDR" :
        rend.mode == HDRPLAY_MODE_SDR   ? "SDR" : "SPLIT",
        rend.mode == HDRPLAY_MODE_SPLIT ? orient_name : "");

    /* ----------------------------------------------------------------
     * Main loop.
     *
     * A single master clock in seconds drives everything. Each source
     * independently advances to the frame in effect at that instant —
     * the last one whose PTS is <= the clock. Two files at different
     * frame rates therefore land on different frame INDICES at the same
     * moment, which is the correct reading of "synchronized" and the
     * reason this is not frame-index lockstep.
     *
     * Playback advances the clock from wall time, pacing against the
     * file's own timestamps so it runs at native speed rather than as
     * fast as the GPU can go. Pause holds the clock still but keeps
     * rendering, so HDR/SDR/split toggles stay interactive while
     * frozen. Resume re-baselines so the pause does not register as
     * "behind".
     * ---------------------------------------------------------------- */
    bool quit   = false;
    bool paused = false;

    double clock_sec  = 0.0;
    double base_wall  = 0.0;
    double base_clock = 0.0;
    bool   rebase     = true;
    /* Redraw only when something changed. Set by any input event and by
     * a source advancing; see the render call at the bottom of the
     * loop for why an unconditional redraw is expensive here. */
    bool   dirty      = true;

    /* Reference source for stepping and for the frame counter: the pane
     * you are looking at. */
    #define REF (renderer_focus_source(&rend))

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            /* Any event may change what should be on screen — resize,
             * moving between displays, focus. Cheaper to redraw than to
             * enumerate which ones matter. */
            dirty = true;
            if (e.type == SDL_EVENT_QUIT) quit = true;

            if (e.type == SDL_EVENT_KEY_DOWN) {
                bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;

                if (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_Q) quit = true;
                if (e.key.key == SDLK_F) {
                    bool fs = (SDL_GetWindowFlags(rend.window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(rend.window, !fs);
                }
                if (e.key.key == SDLK_H) { rend.mode = HDRPLAY_MODE_HDR;   LOG("REND", "mode -> HDR"); }
                if (e.key.key == SDLK_S) { rend.mode = HDRPLAY_MODE_SDR;   LOG("REND", "mode -> SDR"); }
                if (e.key.key == SDLK_P) { rend.mode = HDRPLAY_MODE_SPLIT; LOG("REND", "mode -> SPLIT"); }
                if (e.key.key == SDLK_O) {
                    rend.split_orient = (rend.split_orient + 1) % 3;
                    LOG("REND", "split orientation -> %s",
                        rend.split_orient == HDRPLAY_SPLIT_LR   ? "left/right" :
                        rend.split_orient == HDRPLAY_SPLIT_TB   ? "top/bottom" : "diagonal");
                }
                if (e.key.key == SDLK_SPACE) {
                    paused = !paused;
                    rend.paused = paused;
                    if (!paused) rebase = true;
                    LOG("DEC", "%s", paused ? "paused" : "resumed");
                }
                if (e.key.key == SDLK_L) {
                    loop_at_eof = !loop_at_eof;
                    rend.loop_enabled = loop_at_eof;
                    LOG("DEC", "loop %s", loop_at_eof ? "ON" : "OFF");
                }
                if (e.key.key == SDLK_R) {
                    for (int i = 0; i < n_sources; i++) {
                        decoder_seek_start(&sources[i].dec);
                        source_flush(&sources[i]);
                    }
                    clock_sec = 0.0;
                    rebase = true;
                    LOG("DEC", "restarted from beginning");
                }

                /* Seek. Arrows keep their existing +/-10s meaning, which
                 * is why panning below is on shift+arrows and drag. */
                if (!shift && (e.key.key == SDLK_RIGHT || e.key.key == SDLK_LEFT)) {
                    double delta = (e.key.key == SDLK_RIGHT) ? 10.0 : -10.0;
                    double target = clock_sec + delta;
                    if (target < 0.0) target = 0.0;
                    for (int i = 0; i < n_sources; i++) {
                        if (decoder_seek_to(&sources[i].dec, target))
                            source_flush(&sources[i]);
                    }
                    clock_sec = target;
                    rebase = true;
                    LOG("DEC", "seek %+.0fs -> %.2f", delta, target);
                }

                /* Frame stepping. Both step keys pause first: stepping
                 * while the clock is running would immediately be undone
                 * by the next advance. */
                if (e.key.key == SDLK_PERIOD || e.key.key == SDLK_COMMA) {
                    if (!paused) { paused = true; rend.paused = true; }
                    bool fwd = (e.key.key == SDLK_PERIOD);
                    double t = fwd ? source_step_forward(&sources[REF])
                                   : source_step_back(&sources[REF]);
                    if (!isnan(t)) {
                        /* The reference moved; pull everyone else to the
                         * same instant. */
                        clock_sec = t;
                        for (int i = 0; i < n_sources; i++)
                            if (i != REF) source_advance_to(&sources[i], clock_sec);
                        rebase = true;
                    } else {
                        LOG("DEC", "step %s unavailable", fwd ? "forward" : "back");
                    }
                }

                if (e.key.key == SDLK_M) {
                    rend.probe_active = !rend.probe_active;
                    LOG("REND", "luminance probe %s", rend.probe_active ? "ON" : "OFF");
                }
                if (e.key.key == SDLK_I) {
                    rend.hud_hidden = !rend.hud_hidden;
                    LOG("REND", "status HUD %s", rend.hud_hidden ? "HIDDEN" : "SHOWN");
                }
                /* T rotates the focused pane. With two files un-soloed
                 * that is pane A; press 2 first to reach pane B. */
                if (e.key.key == SDLK_T) {
                    int f = renderer_focus_source(&rend);
                    rend.rotation[f] = (rend.rotation[f] + 90) % 360;
                    LOG("REND", "rotate %s -> %d°",
                        sources[f].label, rend.rotation[f]);
                }
                /* A toggles the accumulated panel, shift-A resets it.
                 * The modifier test is explicit because SDLK_A matches
                 * regardless of shift — without it, shift-A would toggle
                 * AND reset. */
                if (e.key.key == SDLK_A) {
                    if (shift) {
                        for (int i = 0; i < n_sources; i++)
                            session_stats_reset(&sources[i].session);
                        LOG("STAT", "session statistics reset");
                    } else {
                        rend.session_panel = !rend.session_panel;
                        LOG("STAT", "session panel %s",
                            rend.session_panel ? "SHOWN" : "HIDDEN");
                    }
                }

                /* Comparison controls. Inert with one file open. */
                if (n_sources > 1) {
                    if (e.key.key == SDLK_0) {
                        rend.solo = -1; LOG("REND", "compare A|B");
                    }
                    if (e.key.key == SDLK_1) {
                        rend.solo = 0;  LOG("REND", "solo %s", sources[0].label);
                    }
                    if (e.key.key == SDLK_2) {
                        rend.solo = 1;  LOG("REND", "solo %s", sources[1].label);
                    }
                    if (e.key.key == SDLK_X) {
                        rend.swapped = !rend.swapped;
                        LOG("REND", "swap -> %s | %s",
                            sources[rend.swapped ? 1 : 0].label,
                            sources[rend.swapped ? 0 : 1].label);
                    }
                }

                /* Zoom. Fit hides exactly the detail a comparison is
                 * for, so 1:1 is a first-class control rather than a
                 * convenience. */
                if (e.key.key == SDLK_Z) {
                    rend.zoom = (rend.zoom > 0.0f) ? 0.0f : 1.0f;
                    LOG("REND", "zoom -> %s", rend.zoom > 0.0f ? "1:1" : "fit");
                }
                if (e.key.key == SDLK_EQUALS || e.key.key == SDLK_PLUS) {
                    rend.zoom = (rend.zoom <= 0.0f) ? 1.0f : rend.zoom * 2.0f;
                    if (rend.zoom > 8.0f) rend.zoom = 8.0f;
                    LOG("REND", "zoom -> %.1f:1", rend.zoom);
                }
                if (e.key.key == SDLK_MINUS) {
                    rend.zoom = (rend.zoom <= 1.0f) ? 0.0f : rend.zoom * 0.5f;
                    LOG("REND", "zoom -> %s", rend.zoom > 0.0f ? "in" : "fit");
                }
                /* Keyboard pan, since the arrows are taken by seek. */
                if (shift && rend.zoom > 0.0f) {
                    float d = 0.05f;
                    if (e.key.key == SDLK_LEFT)  rend.pan_x -= d;
                    if (e.key.key == SDLK_RIGHT) rend.pan_x += d;
                    if (e.key.key == SDLK_UP)    rend.pan_y -= d;
                    if (e.key.key == SDLK_DOWN)  rend.pan_y += d;
                    if (rend.pan_x < 0) rend.pan_x = 0; if (rend.pan_x > 1) rend.pan_x = 1;
                    if (rend.pan_y < 0) rend.pan_y = 0; if (rend.pan_y > 1) rend.pan_y = 1;
                }
            }

            /* Drag pans; bare motion feeds the probe. The two coexist
             * because one needs a held button and the other does not. */
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                int w = 0, h = 0;
                SDL_GetWindowSizeInPixels(rend.window, &w, &h);
                float sx = 1.0f, sy = 1.0f;
                int lw = 0, lh = 0;
                SDL_GetWindowSize(rend.window, &lw, &lh);
                if (lw > 0 && lh > 0) { sx = (float)w / lw; sy = (float)h / lh; }

                if ((e.motion.state & SDL_BUTTON_LMASK) && rend.zoom > 0.0f) {
                    /* Pan is in normalized source units, so the drag has
                     * to be divided by how much of the source is on
                     * screen — otherwise it accelerates with zoom. */
                    rend.pan_x -= (float)e.motion.xrel / (w > 0 ? w : 1) / rend.zoom;
                    rend.pan_y -= (float)e.motion.yrel / (h > 0 ? h : 1) / rend.zoom;
                    if (rend.pan_x < 0) rend.pan_x = 0; if (rend.pan_x > 1) rend.pan_x = 1;
                    if (rend.pan_y < 0) rend.pan_y = 0; if (rend.pan_y > 1) rend.pan_y = 1;
                } else {
                    rend.probe_x = (int)(e.motion.x * sx);
                    rend.probe_y = (int)(e.motion.y * sy);
                    rend.probe_win_w = w;
                    rend.probe_win_h = h;
                }
            }
        }
        if (quit) break;

        if (!paused) {
            /* Advance the master clock from wall time. */
            double now = now_seconds();

            if (rebase) {
                base_wall  = now;
                base_clock = clock_sec;
                rebase = false;
            }
            clock_sec = base_clock + (now - base_wall);

            bool all_eof = true;
            for (int i = 0; i < n_sources; i++) {
                if (source_advance_to(&sources[i], clock_sec)) dirty = true;
                /* A source that has run out holds its last frame rather
                 * than going black; only quit when EVERY source is done,
                 * so a short B does not cut a longer A short. */
                if (!sources[i].eof) all_eof = false;
            }

            if (all_eof) {
                LOG("DEC", "EOF");
                if (loop_at_eof) {
                    for (int i = 0; i < n_sources; i++) {
                        decoder_seek_start(&sources[i].dec);
                        source_flush(&sources[i]);
                    }
                    clock_sec = 0.0;
                    rebase = true;
                    continue;
                }
                quit = true;
            }

            /* Pace against the NEXT frame, not the one already shown.
             *
             * The shown frame is by construction at or before `now`, so
             * pacing off it yields a non-positive delay and never
             * sleeps. The loop then free-runs, re-mapping and
             * re-uploading every source's frame on every iteration —
             * with two 1728x2304 sources that saturates memory
             * bandwidth and the visible frame rate collapses to a few
             * fps. Sleeping until the next frame is due keeps the loop
             * at roughly one iteration per source frame. */
            double next = source_peek_next_sec(&sources[REF]);
            if (!isnan(next)) {
                double delay = (base_wall + (next - base_clock)) - now_seconds();
                if (delay > 0.0 && delay < 1.0) {
                    struct timespec ts = {
                        .tv_sec  = (time_t)delay,
                        .tv_nsec = (long)((delay - (double)(time_t)delay) * 1e9),
                    };
                    nanosleep(&ts, NULL);
                } else if (delay < -0.050) {
                    LOGV("DEC", "behind=%.0fms (next=%.3fs)", -delay * 1000.0, next);
                }
            }
        } else {
            /* Paused: idle rather than spin. Toggles still redraw
             * because they set `dirty`. */
            SDL_Delay(16);
        }

        bool have_any = false;
        for (int i = 0; i < n_sources; i++) if (sources[i].shown) have_any = true;

        /* Only re-render when something actually changed. Every render
         * re-uploads each source's frame to the GPU, so redrawing an
         * unchanged pause screen at 60Hz costs the same bandwidth as
         * playback for no benefit. */
        if (have_any && dirty) {
            renderer_render(&rend, sources, n_sources);
            dirty = false;
        } else if (!have_any) {
            SDL_Delay(16);
        }
    }

    /* Dump what accumulated over the session, per file. Printed on exit
     * because the interesting numbers — peaks that latched somewhere
     * mid-clip — are exactly the ones you cannot recover once the
     * window closes. Coverage is stated so a partial watch is never
     * mistaken for a full scan. */
    for (int i = 0; i < n_sources; i++) {
        char title[512];
        snprintf(title, sizeof(title), "session (playback)  %s", sources[i].label);
        analyze_print_session(&sources[i].session,
                              sources[i].dec.has_cll ? sources[i].dec.cll_max : -1,
                              sources[i].dec.has_cll ? sources[i].dec.cll_avg : -1,
                              title);
    }

    renderer_close(&rend);
    for (int i = 0; i < n_sources; i++) source_close(&sources[i]);
    return 0;
}
