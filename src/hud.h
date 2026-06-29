#ifndef HDRPLAY_HUD_H
#define HDRPLAY_HUD_H

#include <libplacebo/renderer.h>

typedef struct Renderer Renderer;

/* Three logically-independent overlays the HUD can produce. Renderer
 * picks which ones to attach to each pl_render_image call depending on
 * mode and split orientation — see comment in hud_prepare. */
typedef struct {
    struct pl_overlay status;     /* multi-line panel, top-left           */
    struct pl_overlay hdr_label;  /* big "HDR" badge (split mode only)    */
    struct pl_overlay sdr_label;  /* big "SDR" badge (split mode only)    */
    int               win_w, win_h;
} HudOverlays;

/* Build / refresh overlay textures for the current frame, sized to a
 * window of (win_w × win_h). After this returns, the caller assigns
 * the appropriate `pl_overlay`s into `target->overlays` BEFORE calling
 * pl_render_image. Overlays attached AFTER a render are ignored —
 * that was the bug that hid the original HUD. */
void hud_prepare(Renderer *r, int win_w, int win_h, HudOverlays *out);

/* Release HUD slot textures. Must be called before pl_vulkan_destroy
 * — otherwise the slot textures survive into vulkan teardown and
 * libplacebo's allocator reports them as leaked. */
void hud_close(pl_gpu gpu);

#endif
