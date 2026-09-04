#ifndef HDRPLAY_LAYOUT_H
#define HDRPLAY_LAYOUT_H

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Render layout planning.                                             */
/*                                                                     */
/* Every decision about HOW a frame gets composited — how many passes, */
/* which source feeds each, what window rect it lands in, what part of */
/* the source is visible, which intermediates to prepare, and where    */
/* each HUD overlay attaches — is computed here as plain data, with no */
/* GPU and no libplacebo types involved.                               */
/*                                                                     */
/* The point is testability. The render path needs a GPU, so its       */
/* behaviour cannot be asserted in CI; the DECISIONS can. In particular */
/* single-file rendering must not change as two-file support lands, and */
/* pinning the one-source plan in a test is what enforces that, rather  */
/* than reviewer vigilance.                                            */
/* ------------------------------------------------------------------ */

/* Alpha mask baked into an SDR/second-source intermediate. Lives here
 * rather than inside the Renderer struct (where it was previously
 * declared, producing a -Wmissing-declarations warning on every
 * translation unit that included renderer.h). */
enum AlphaMaskMode {
    ALPHA_MASK_NONE  = 0,  /* unused / not yet computed              */
    ALPHA_MASK_FULL  = 1,  /* opaque everywhere (full SDR mode)      */
    ALPHA_MASK_LR    = 2,  /* alpha 0 on left, 1 on right            */
    ALPHA_MASK_TB    = 3,  /* alpha 0 on top, 1 on bottom            */
    ALPHA_MASK_DIAG  = 4,  /* smoothstep diagonal                    */
};

typedef enum {
    HDRPLAY_MODE_HDR = 0,
    HDRPLAY_MODE_SDR,
    HDRPLAY_MODE_SPLIT,
} HdrplayMode;

typedef enum {
    HDRPLAY_SPLIT_LR = 0,
    HDRPLAY_SPLIT_TB,
    HDRPLAY_SPLIT_DIAG,
    HDRPLAY_SPLIT_WIPE_LR,  /* two-source full-frame left/right wipe */
    HDRPLAY_SPLIT_WIPE_TB,  /* two-source full-frame top/bottom wipe */
} HdrplaySplitOrient;

typedef struct { float x0, y0, x1, y1; } LayoutRect;

/* What an overlay slot refers to. The renderer maps these onto actual
 * pl_overlay structs; layout only decides which appear where. */
typedef enum {
    LAYOUT_OV_INTERMEDIATE,  /* source `src`'s intermediate texture   */
    LAYOUT_OV_STATUS,        /* top-left status panel                 */
    LAYOUT_OV_SESSION,       /* accumulated-statistics panel          */
    LAYOUT_OV_LABEL_A,       /* badge for the first pane              */
    LAYOUT_OV_LABEL_B,       /* badge for the second pane             */
} LayoutOverlayKind;

typedef struct {
    LayoutOverlayKind kind;
    int        src;          /* INTERMEDIATE only: which source       */
    LayoutRect dst;          /* window-space destination              */
} LayoutOverlay;

#define LAYOUT_MAX_PASSES   2
#define LAYOUT_MAX_INTER    2
#define LAYOUT_MAX_OVERLAYS 6

/* An intermediate render, performed before any swapchain pass. */
typedef struct {
    int  src;                /* which source feeds it                 */
    int  mask;               /* enum AlphaMaskMode baked into alpha   */
    bool sdr;                /* true = SDR treatment, false = HDR     */
    LayoutRect dst;          /* window-space rect the content lands in.
                              * The texture is window-sized, so this is
                              * both the render target crop and the
                              * overlay's source rect — a 1:1 map, which
                              * is what keeps the pre-baked alpha mask
                              * aligned with window space. */
    LayoutRect image_crop;   /* source-space crop; all-zero = full.
                              * Must match what `dst` was sized from, or
                              * the intermediate stretches. */
} LayoutInter;

typedef struct {
    int        src;          /* which source to render                */
    LayoutRect target_crop;  /* destination rect in window pixels     */
    LayoutRect image_crop;   /* source-space crop; all-zero = full    */
    float      scale;        /* screen pixels per source pixel; 1.0
                              * is exactly 1:1. Surfaced for the HUD  */
    LayoutOverlay ov[LAYOUT_MAX_OVERLAYS];
    int        n_ov;
} LayoutPass;

typedef struct {
    LayoutInter inter[LAYOUT_MAX_INTER];
    int         n_inter;
    LayoutPass  pass[LAYOUT_MAX_PASSES];
    int         n_pass;
    const char *name;        /* short label for logging               */
} LayoutPlan;

/* Inputs. Grouped in a struct because the argument list would
 * otherwise be a dozen positional parameters that are easy to
 * transpose silently. */
typedef struct {
    HdrplayMode        mode;
    HdrplaySplitOrient orient;

    int   n_sources;         /* 1 or 2                                */
    int   solo;              /* -1 = compare both; else source index  */
    bool  swapped;           /* B on the left                         */

    int   win_w, win_h;

    /* Per-source geometry, as decoded. The two files need not match:
     * comparing a 1080p encode against a 4K master is an ordinary job.
     * layout picks the larger by area as the REFERENCE and expresses
     * zoom against it, so both panes always show the same region of the
     * scene rather than the same number of pixels. */
    int   src_w[2], src_h[2];

    /* Zoom / pan. zoom <= 0 or 1.0 with no pan means "fit". pan_x/y are
     * in normalized source units, 0.5,0.5 = centred. */
    float zoom;
    float pan_x, pan_y;

    bool  hud_hidden;
    bool  session_panel;
} LayoutInput;

/* Compute the plan. Pure: same inputs always give the same output, no
 * allocation, no global state. */
void layout_plan(const LayoutInput *in, LayoutPlan *out);

/* Runtime O-key order. Two-source comparison includes the two full-frame
 * rectangular wipes; single-source/solo keeps the original three modes. */
HdrplaySplitOrient layout_next_split_orient(HdrplaySplitOrient current,
                                             bool two_source_compare);

/* Place a source inside a pane with its aspect ratio preserved.
 *
 * This is the one place geometry is decided. It returns the visible
 * source region and the window rect that region is drawn into, and the
 * two are the same rectangle scaled by a single factor — so the drawn
 * aspect always equals the source aspect, in every mode, at every window
 * size, for every split. There is no separate letterbox pass that some
 * code path could miss.
 *
 * `zoom` <= 0 means fit: the largest scale that gets the whole frame
 * into the pane. `zoom` > 0 is screen pixels per REFERENCE source pixel,
 * so 1.0 is exactly 1:1 (against the physical framebuffer, which is what
 * matters when the question is whether a compression artifact is real).
 * Passing the source's own size as the reference makes zoom mean 1:1 on
 * that source.
 *
 * `align_x`/`align_y` decide where the leftover space in the pane goes:
 * 0 = left/top, 0.5 = centred, 1 = right/bottom. A two-pane split aligns
 * each image toward the seam, so the pair meets in the middle rather
 * than being pushed apart by the sum of their inner margins.
 *
 * Returns the scale actually used, for the HUD to report.
 *
 * `out_image` is all-zero when the whole frame is visible, preserving
 * the convention that lets the renderer leave pl_frame.crop untouched. */
float layout_fit_pane(int src_w, int src_h, int ref_w, int ref_h,
                      LayoutRect pane, float zoom, float pan_x, float pan_y,
                      float align_x, float align_y,
                      LayoutRect *out_image, LayoutRect *out_target);

/* Visible source rect for a given zoom/pan, in the pixels of a source
 * of size (src_w, src_h). A thin wrapper over layout_fit_pane that
 * discards the target rect; kept because the probe needs the same
 * mapping to turn a window coordinate back into a source pixel.
 *
 * `ref_w`/`ref_h` are the reference geometry that zoom is expressed
 * against. Passing the source's own size makes zoom mean "1:1 source
 * pixels"; passing the larger of two sources makes both panes cover the
 * same scene region, which is what a comparison needs — otherwise at
 * 1:1 a 1080p pane would show four times the area of a 4K one. */
LayoutRect layout_image_crop_ref(int src_w, int src_h,
                                 int ref_w, int ref_h,
                                 int dst_w, int dst_h,
                                 float zoom, float pan_x, float pan_y);

/* Convenience: reference geometry == the source's own. */
LayoutRect layout_image_crop(int src_w, int src_h, int dst_w, int dst_h,
                             float zoom, float pan_x, float pan_y);

/* Index of the larger source by pixel area. */
int layout_reference_source(const LayoutInput *in);

/* Frame dimensions as DISPLAYED, given a clockwise rotation in degrees.
 * 90 and 270 swap the axes; 0 and 180 pass through.
 *
 * Everything layout decides — which source is the zoom reference, how a
 * pane letterboxes, whether portrait content should default to a
 * top/bottom split — has to reason about the frame the way the viewer
 * sees it, not the way the encoder stored it. Callers therefore run
 * decoded dimensions through here before filling in src_w/src_h.
 *
 * `rot` need not be normalized; anything that is not 90 or 270 mod 360
 * is treated as no swap. */
void layout_rotated_dims(int rot, int w, int h, int *out_w, int *out_h);

/* Undo a clockwise rotation on a normalized ([0,1]) coordinate.
 *
 * libplacebo's image rotation takes a source point (u,v) to the
 * displayed point (1-v, u) at 90° CW. The probe needs that run
 * backwards: it knows where the cursor is on screen and wants the pixel
 * in the frame's own, unrotated space. Getting it wrong reports a real
 * pixel from the wrong part of the frame — plausible-looking, and wrong.
 *
 * Lives here rather than in renderer.c so it is reachable from the
 * layout tests; it is the same geometry as the dimension swap above. */
void layout_unrotate_norm(int rot, double *x, double *y);

#endif
