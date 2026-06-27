#ifndef HDRPLAY_RENDERER_H
#define HDRPLAY_RENDERER_H

#include <stdbool.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/vulkan.h>

struct SDL_Window;

typedef struct Renderer {
    struct SDL_Window *window;

    pl_log              pl_log;
    pl_vk_inst          vk_inst;
    pl_vulkan           vulkan;
    pl_swapchain        swapchain;
    pl_renderer         renderer;       /* HDR path / main render        */
    pl_renderer         renderer_sdr;   /* SDR-to-intermediate path      */

    /* Persistent plane textures, recycled across frames. libplacebo
     * (re)creates them on first use; we just have to keep the array
     * alive so pl_unmap_avframe doesn't have to destroy them. */
    pl_tex              plane_tex[4];

    /* SDR-overlay intermediate. We render the SDR pass into this RGBA
     * texture (with blend_params that preserve dst.alpha). The alpha
     * channel is pre-filled with a mode-specific mask: FULL (everywhere
     * opaque = full SDR), LR / TB (hard edge halves), or DIAG (smooth
     * antialiased diagonal). The texture then composites over the HDR
     * pass via pl_overlay — and crucially, overlay composition bypasses
     * libplacebo's tone-mapping pipeline, so absolute PQ-encoded SDR
     * brightness (≈ 203 nits) actually lands at 203 nits on the panel
     * instead of being renormalized to swap-chain peak. */
    pl_tex              diag_tex;
    int                 diag_tex_w, diag_tex_h;
    int                 diag_tex_mask_mode;  /* see enum AlphaMaskMode below */

    /* State surfaced to main loop and HUD. */
    bool   display_hdr_capable;     /* SDL says display is in HDR mode */
    float  display_sdr_white;       /* current SDR white level, nits   */
    float  display_hdr_headroom;    /* current EDR headroom, ratio     */

    /* Output mode — toggled at runtime via `S`/`P` keys. */
    enum {
        HDRPLAY_MODE_HDR     = 0,   /* full BT.2020 + PQ, panel peak   */
        HDRPLAY_MODE_SDR     = 1,   /* BT.709 + bt1886, 100-nit ceil   */
        HDRPLAY_MODE_SPLIT   = 2,   /* HDR + SDR side-by-side          */
    } mode;

    /* Orientation of the SPLIT view's divider — toggled via `O` key. */
    enum {
        HDRPLAY_SPLIT_LR     = 0,   /* vertical divider: HDR=left, SDR=right */
        HDRPLAY_SPLIT_TB     = 1,   /* horizontal divider: HDR=top, SDR=bottom */
        HDRPLAY_SPLIT_DIAG   = 2,   /* diagonal: HDR=upper-left triangle, SDR=lower-right */
    } split_orient;

    /* Playback state surfaced to HUD. Main loop owns these flags and
     * pokes them in so the on-screen overlay reflects current state. */
    bool   paused;
    bool   loop_enabled;

    /* Alpha-mask shape pre-filled into diag_tex.alpha. Selected per
     * frame based on r->mode + r->split_orient. */
    enum AlphaMaskMode {
        ALPHA_MASK_NONE  = 0,  /* unused / not yet computed */
        ALPHA_MASK_FULL  = 1,  /* opaque everywhere (full SDR mode) */
        ALPHA_MASK_LR    = 2,  /* alpha 0 on left, 1 on right */
        ALPHA_MASK_TB    = 3,  /* alpha 0 on top, 1 on bottom */
        ALPHA_MASK_DIAG  = 4,  /* smoothstep diagonal */
    };

    /* SDR-mode tone-map ceiling, in nits. <= 0 means "track the OS
     * current SDR-white reference" (so SDR mode matches the perceptual
     * brightness of macOS's own SDR layer compositing, ≈ ffplay). > 0
     * pins to a specific value (100 = strict BT.2100 spec, 203 = Apple
     * HDR Video reference, 500 = Apple Display preset, etc.). */
    float  sdr_peak_override;
    float  sdr_peak_effective;   /* what we actually used last frame */

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
} Renderer;

/* display_index: 0-based index into the list SDL exposes, or -1 to use
 * the OS default. List can be inspected before init via renderer_list_displays. */
bool renderer_init(Renderer *r, int width, int height, const char *title, int display_index);
void renderer_list_displays(void);
bool renderer_render_avframe(Renderer *r, struct AVFrame *frame);
void renderer_update_display_state(Renderer *r);
void renderer_close(Renderer *r);

#endif
