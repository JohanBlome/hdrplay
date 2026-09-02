#ifndef HDRPLAY_RENDERER_H
#define HDRPLAY_RENDERER_H

#include <stdbool.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/gamut_mapping.h>
#include "probe.h"
#include "layout.h"

struct SDL_Window;
struct Source;

typedef struct Renderer {
    struct SDL_Window *window;

    pl_log              pl_log;
    pl_vk_inst          vk_inst;
    pl_vulkan           vulkan;
    pl_swapchain        swapchain;
    pl_renderer         renderer;       /* swapchain passes, always      */
    pl_renderer         renderer_inter; /* intermediates, always         */

    /* Per-source GPU state. Two sources means two sets of upload
     * textures and two intermediates: sharing one intermediate across
     * passes would be a hazard, since both are queued before either
     * executes.
     *
     * Kept here rather than on Source so source.h stays free of
     * libplacebo — Source is decode state, this is GPU state. */
    struct {
        /* Persistent plane textures, recycled across frames. libplacebo
         * (re)creates them on first use; we just keep the array alive
         * so pl_unmap_avframe doesn't have to destroy them. */
        pl_tex plane_tex[4];

        /* Overlay intermediate. The pass renders into this RGBA texture
         * (with blend_params that preserve dst.alpha). The alpha channel
         * is pre-filled with a mask: FULL (opaque everywhere), LR / TB
         * (hard edge halves), or DIAG (smooth antialiased diagonal). It
         * then composites via pl_overlay — and crucially, overlay
         * composition bypasses libplacebo's tone-mapping pipeline, so
         * absolute PQ-encoded SDR brightness (~203 nits) actually lands
         * at 203 nits on the panel instead of being renormalized to
         * swapchain peak. */
        pl_tex inter_tex;
        int    inter_w, inter_h, inter_mask;
        /* Window-space rect the image occupies inside the texture. The
         * mask is rebaked when this moves, since alpha outside it is
         * forced to 0 to letterbox the pane. */
        struct pl_rect2df inter_dst;
        bool   mapped;
        struct pl_frame image;
    } slot[2];

    /* State surfaced to main loop and HUD. */
    bool   display_hdr_capable;     /* SDL says display is in HDR mode */
    float  display_sdr_white;       /* current SDR white level, nits   */
    float  display_hdr_headroom;    /* current EDR headroom, ratio     */

    /* Output mode and split orientation. Defined in layout.h, which
     * owns every decision that depends on them. */
    HdrplayMode        mode;
    HdrplaySplitOrient split_orient;

    /* Two-file comparison state. n_sources == 1 keeps every code path
     * identical to single-file playback; the rest is inert. */
    int    n_sources;
    int    solo;            /* -1 = compare both, else source index    */
    bool   swapped;         /* B on the left                           */
    float  zoom;            /* <= 0 = fit, 1.0 = 1:1 source pixels     */
    float  pan_x, pan_y;    /* normalized centre of the visible region */

    /* Per-source clockwise rotation in degrees: 0, 90, 180 or 270.
     * View state rather than decode state — the T key changes it
     * between frames — so it lives here alongside zoom and solo and
     * never reaches Source. See --rotate and docs/plans/
     * 2026-08-31-input-rotation-design.md. */
    int    rotation[2];

    /* Playback state surfaced to HUD. Main loop owns these flags and
     * pokes them in so the on-screen overlay reflects current state. */
    bool   paused;
    bool   loop_enabled;

    /* SDR-mode tone-map ceiling, in nits. <= 0 means "track the OS
     * current SDR-white reference" (so SDR mode matches the perceptual
     * brightness of macOS's own SDR layer compositing, ≈ ffplay). > 0
     * pins to a specific value (100 = strict BT.2100 spec, 203 = Apple
     * HDR Video reference, 500 = Apple Display preset, etc.). */
    float  sdr_peak_override;
    float  sdr_peak_effective;   /* what we actually used last frame */

    /* SDR-pass saturation gain. 1.0 = libplacebo default (which applies
     * perceptual desaturation during tone-mapping to keep hue stable
     * across brightness changes — looks more natural per spec, but
     * less vivid than macOS's SDR layer composition / QuickTime). 1.2
     * default roughly matches QuickTime saturation; 1.0 is "let
     * libplacebo do its thing", >1.5 is over-saturated. */
    float  sdr_saturation;

    /* SDR-pass gamut-mapping function. Defaults to pl_gamut_map_perceptual
     * (BT.2407 perceptual rolloff — matches what HDR-capable displays do
     * internally when handed BT.2020 content for a BT.709 panel, so it
     * approximates "what an SDR display would show"). NULL = no gamut
     * mapping (BT.2020 colors clip however libplacebo defaults). Other
     * useful values: &pl_gamut_map_clip (hard clip, oversaturated edges),
     * &pl_gamut_map_relative (relative colorimetric — closest to OS color
     * management's BT.2020→BT.709 conversion). */
    const struct pl_gamut_map_function *sdr_gamut_map;

    /* SDR-pass dynamic-range cap in stops. Used to compute
     *     target.min_luma = sdr_peak / 2^cap
     * which makes libplacebo compress sub-floor source detail up to
     * the SDR black floor (the third axis of HDR-advantage demo,
     * after peak and gamut).
     *
     * Default 12.0 — keeps the floor at ~0.1 nits (BT.1886 reference
     * black on a modern dim-room display) when paired with the
     * 500-nit default sdr_peak. 10.0 would only be right for strict
     * 100-nit BT.1886 (legacy SDR displays).
     *
     * Override with --sdr-dr-stops. */
    float sdr_dr_stops_cap;

    /* Source-video frame number we're currently displaying. Computed
     * from PTS × fps in main.c after each successful decode, so it
     * stays correct across seeks/restarts (unlike a render-side
     * counter, which would just count refreshes). -1 = no frame
     * decoded yet. */
    int current_frame_no;

    /* HUD visibility toggle. The status panel (top-left, multi-line)
     * can occlude image content in some clips — let the user hide it
     * with 'I'. The HDR/SDR split-mode badges are NOT gated by this
     * because they're load-bearing for telling the panes apart. */
    bool hud_hidden;

    /* Luminance probe — main loop pokes mouse coords (window-relative,
     * in window pixels), HUD draws a crosshair and tells `decoder`-land
     * to sample the source pixel and report nominal nits. -1 = disabled. */
    int    probe_x, probe_y;
    int    probe_win_w, probe_win_h;     /* window size at last sample   */
    bool   probe_active;
    double probe_nits;                    /* result, NaN if unavailable   */
    double probe_y_norm;                  /* normalized Y' value (0..1)   */
    double probe_r_nits, probe_g_nits, probe_b_nits;

    /* Last frame's libplacebo decisions, populated via info_callback. */
    char   last_tonemap[128];
    char   last_output_csp[128];
    char   last_source_csp[128];   /* primaries/transfer/peak from AVFrame */
    int    last_num_passes;

    /* Screen pixels per source pixel for the focused pane, as last
     * planned. Reported by the HUD: "is this actually 1:1?" is not
     * answerable by eye, and at anything below 1:1 a resampler sits
     * between you and the artifact you are trying to judge. */
    float  last_scale;

    /* Per-frame source brightness statistics. Updated each frame by
     * probe_frame_stats(). Lets the HUD show peak/dr/% above SDR so
     * the user can see at a glance whether the current frame actually
     * has HDR-worthy content (highlights above the SDR ceiling). */
    FrameStats frame_stats;
    bool       frame_stats_valid;

    /* Session-wide accumulation of the above. Owned by main.c (it knows
     * the stream timebase and duration); the renderer only reads it for
     * the HUD and feeds each frame in. NULL when unavailable. */
    struct SessionStats *session;
    bool   session_panel;       /* 'A' toggles the accumulated panel   */

    /* HDR10 static metadata the container DECLARES, copied from the
     * decoder so the HUD can print measured-vs-declared side by side.
     * cll_max is MaxCLL, cll_avg is MaxFALL. */
    bool   has_declared_cll;
    int    declared_cll_max, declared_cll_avg;
} Renderer;

/* display_index: 0-based index into the list SDL exposes, or -1 to use
 * the OS default. List can be inspected before init via renderer_list_displays. */
bool renderer_init(Renderer *r, int width, int height, const char *title, int display_index);
void renderer_list_displays(void);
/* Render the current state of every source into one window frame.
 * `n` is 1 or 2; with 1 this reduces to exactly the previous
 * single-file path (see tests/test_layout.c, which pins that). */
bool renderer_render(Renderer *r, struct Source *sources, int n);

/* Which source the HUD, probe, statistics and frame stepping describe:
 * the left/top pane in a comparison, or the soloed file. */
int  renderer_focus_source(const Renderer *r);
void renderer_update_display_state(Renderer *r);
void renderer_close(Renderer *r);

#endif
