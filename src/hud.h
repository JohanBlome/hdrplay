#ifndef HDRPLAY_HUD_H
#define HDRPLAY_HUD_H

#include <stdbool.h>
#include <libplacebo/renderer.h>

#include "layout.h"

typedef struct Renderer Renderer;
struct Source;

/* Logically-independent overlays the HUD can produce. All go into the
 * single swapchain render — see the comment in hud_prepare. */
typedef struct {
    struct pl_overlay status;     /* multi-line panel, top-left           */
    struct pl_overlay label_a;    /* badge for the first pane             */
    struct pl_overlay label_b;    /* badge for the second pane            */
    struct pl_overlay session;    /* accumulated statistics panel         */
    bool has_status, has_label_a, has_label_b, has_session;
    int  win_w, win_h;
} HudOverlays;

/* Build / refresh overlay textures for the current frame, sized to a
 * window of (win_w × win_h). After this returns, the caller assigns
 * the appropriate `pl_overlay`s into `target->overlays` BEFORE calling
 * pl_render_image. Overlays attached AFTER a render are ignored —
 * that was the bug that hid the original HUD. */
/* `plan` decides which panels exist and where they go; hud only draws
 * them. Keeping placement in layout.c means the routing tests and the
 * renderer agree by construction rather than by two copies of the same
 * arithmetic. */
void hud_prepare(Renderer *r, struct Source *sources, int n,
                 const LayoutPlan *plan, int win_w, int win_h,
                 HudOverlays *out);

/* Release HUD slot textures. Must be called before pl_vulkan_destroy
 * — otherwise the slot textures survive into vulkan teardown and
 * libplacebo's allocator reports them as leaked. */
void hud_close(pl_gpu gpu);

#endif
