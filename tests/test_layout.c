/* Layout planning tests.
 *
 * The first block is the important one: it pins single-file rendering
 * against the behaviour that existed before two-file support. If adding
 * a second source perturbs the one-source plan, these fail. That is the
 * mechanism enforcing "single-file playback works as it does today" —
 * the render path itself needs a GPU and cannot be asserted in CI.
 */
#include "layout.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, fmt, ...) do {                                  \
    if (cond) printf("  ok    " fmt "\n", ##__VA_ARGS__);           \
    else { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); fails++; }   \
} while (0)

#define WIN_W 1920
#define WIN_H 1080

static LayoutInput base_input(void)
{
    return (LayoutInput){
        .mode = HDRPLAY_MODE_HDR,
        .orient = HDRPLAY_SPLIT_LR,
        .n_sources = 1,
        .solo = -1,
        .swapped = false,
        .win_w = WIN_W, .win_h = WIN_H,
        .src_w = { 3840, 3840 }, .src_h = { 2160, 2160 },
        .zoom = 0.0f, .pan_x = 0.5f, .pan_y = 0.5f,
        .hud_hidden = false,
        .session_panel = false,
    };
}

/* The all-zero rect is layout's "whole frame" sentinel. */
static bool rect_is_zero_t(LayoutRect r)
{
    return r.x0 == 0 && r.y0 == 0 && r.x1 == 0 && r.y1 == 0;
}

static bool rect_eq(LayoutRect r, float x0, float y0, float x1, float y1)
{
    return fabsf(r.x0 - x0) < 0.01f && fabsf(r.y0 - y0) < 0.01f &&
           fabsf(r.x1 - x1) < 0.01f && fabsf(r.y1 - y1) < 0.01f;
}

static bool is_full_crop(LayoutRect r)
{
    return r.x0 == 0 && r.y0 == 0 && r.x1 == 0 && r.y1 == 0;
}

static int count_ov(const LayoutPass *p, LayoutOverlayKind k)
{
    int n = 0;
    for (int i = 0; i < p->n_ov; i++) if (p->ov[i].kind == k) n++;
    return n;
}

/* Every overlay must appear in exactly one pass across the whole plan.
 * Attaching one to two passes composites it twice; at the panel's alpha
 * of 170 the background would blend to ~0.89 instead of 0.667 — visibly
 * darker than every other element. */
static int count_ov_plan(const LayoutPlan *pl, LayoutOverlayKind k)
{
    int n = 0;
    for (int i = 0; i < pl->n_pass; i++) n += count_ov(&pl->pass[i], k);
    return n;
}

/* ------------------------------------------------------------------ */
static void test_single_file_unchanged(void)
{
    puts("single-file plans match pre-existing behaviour");
    LayoutPlan pl;

    /* HDR: one pass, full crop, no intermediate. */
    LayoutInput in = base_input();
    layout_plan(&in, &pl);
    CHECK(pl.n_pass == 1, "HDR: 1 pass (got %d)", pl.n_pass);
    CHECK(pl.n_inter == 0, "HDR: no intermediate (got %d)", pl.n_inter);
    CHECK(pl.pass[0].src == 0, "HDR: renders source 0");
    CHECK(rect_eq(pl.pass[0].target_crop, 0, 0, WIN_W, WIN_H),
          "HDR: full-window target crop");
    CHECK(is_full_crop(pl.pass[0].image_crop),
          "HDR: image crop left untouched (libplacebo default)");
    CHECK(count_ov(&pl.pass[0], LAYOUT_OV_STATUS) == 1, "HDR: status attached");
    CHECK(count_ov(&pl.pass[0], LAYOUT_OV_INTERMEDIATE) == 0,
          "HDR: no intermediate overlay");

    /* SDR: FULL-mask intermediate over an HDR base render. */
    in = base_input();
    in.mode = HDRPLAY_MODE_SDR;
    layout_plan(&in, &pl);
    CHECK(pl.n_pass == 1, "SDR: still 1 pass");
    CHECK(pl.n_inter == 1 && pl.inter[0].mask == ALPHA_MASK_FULL &&
          pl.inter[0].sdr && pl.inter[0].src == 0,
          "SDR: one FULL-mask SDR intermediate from source 0");
    CHECK(pl.pass[0].ov[0].kind == LAYOUT_OV_INTERMEDIATE,
          "SDR: intermediate composited FIRST, before HUD text");

    /* SPLIT: mask follows orientation, badges appear. */
    struct { HdrplaySplitOrient o; int mask; const char *n; } cases[] = {
        { HDRPLAY_SPLIT_LR,   ALPHA_MASK_LR,   "LR"   },
        { HDRPLAY_SPLIT_TB,   ALPHA_MASK_TB,   "TB"   },
        { HDRPLAY_SPLIT_DIAG, ALPHA_MASK_DIAG, "DIAG" },
    };
    for (size_t i = 0; i < 3; i++) {
        in = base_input();
        in.mode = HDRPLAY_MODE_SPLIT;
        in.orient = cases[i].o;
        layout_plan(&in, &pl);
        CHECK(pl.n_pass == 1 && pl.n_inter == 1 &&
              pl.inter[0].mask == cases[i].mask,
              "SPLIT-%s: 1 pass, mask %d", cases[i].n, cases[i].mask);
        CHECK(count_ov(&pl.pass[0], LAYOUT_OV_LABEL_A) == 1 &&
              count_ov(&pl.pass[0], LAYOUT_OV_LABEL_B) == 1,
              "SPLIT-%s: both badges attached", cases[i].n);
    }

    /* I hides the status panel and nothing else. */
    in = base_input();
    in.hud_hidden = true;
    layout_plan(&in, &pl);
    CHECK(count_ov_plan(&pl, LAYOUT_OV_STATUS) == 0, "hud_hidden drops status");

    /* Session panel is bottom-right with one source. */
    in = base_input();
    in.session_panel = true;
    layout_plan(&in, &pl);
    CHECK(count_ov_plan(&pl, LAYOUT_OV_SESSION) == 1, "session attached once");
    const LayoutOverlay *s = NULL;
    for (int i = 0; i < pl.pass[0].n_ov; i++)
        if (pl.pass[0].ov[i].kind == LAYOUT_OV_SESSION) s = &pl.pass[0].ov[i];
    CHECK(s && rect_eq(s->dst, WIN_W - 16 - 440, WIN_H - 16 - 200,
                       WIN_W - 16, WIN_H - 16),
          "session panel bottom-right");
}

/* Solo must be indistinguishable from opening that file alone — that is
 * the entire reason solo exists, so the HDR-vs-SDR comparison stays one
 * keypress away in two-file mode. */
static void test_solo_equals_single(void)
{
    puts("solo reproduces the single-file plan exactly");
    HdrplayMode modes[] = { HDRPLAY_MODE_HDR, HDRPLAY_MODE_SDR, HDRPLAY_MODE_SPLIT };
    for (int m = 0; m < 3; m++) {
        LayoutInput one = base_input();
        one.mode = modes[m];
        one.session_panel = true;
        LayoutPlan p1; layout_plan(&one, &p1);

        LayoutInput two = one;
        two.n_sources = 2;
        two.solo = 0;
        LayoutPlan p2; layout_plan(&two, &p2);

        /* name is a literal pointer; compare the rest bytewise. */
        p1.name = p2.name = NULL;
        CHECK(memcmp(&p1, &p2, sizeof(p1)) == 0,
              "mode %d: solo plan byte-identical to single-file", m);
    }

    /* Solo 1 differs only in which source feeds the pass. */
    LayoutInput in = base_input();
    in.n_sources = 2; in.solo = 1;
    LayoutPlan pl; layout_plan(&in, &pl);
    CHECK(pl.n_pass == 1 && pl.pass[0].src == 1, "solo 1 renders source 1");
}

/* ------------------------------------------------------------------ */
static void test_pair_lr_tb(void)
{
    puts("two sources, LR and TB");
    LayoutPlan pl;

    LayoutInput in = base_input();
    in.n_sources = 2;
    layout_plan(&in, &pl);
    CHECK(pl.n_pass == 2, "LR: two passes");
    CHECK(pl.n_inter == 0, "LR HDR: no intermediates — direct render");
    CHECK(pl.pass[0].src == 0 && pl.pass[1].src == 1, "LR: A then B");
    /* A 16:9 source in a half-width pane letterboxes vertically: the
     * pane is 960x1080 but the image can only be 960x540. Filling the
     * pane, which is what this used to do, squeezed every frame 2:1
     * horizontally — the whole reason for the fit rework. */
    CHECK(rect_eq(pl.pass[0].target_crop, 0, 270, WIN_W / 2, 810),
          "LR: pane A is letterboxed inside the left half");
    CHECK(rect_eq(pl.pass[1].target_crop, WIN_W / 2, 270, WIN_W, 810),
          "LR: pane B is letterboxed inside the right half");

    in.swapped = true;
    layout_plan(&in, &pl);
    CHECK(pl.pass[0].src == 1 && pl.pass[1].src == 0,
          "X swaps which source is on the left");

    in = base_input();
    in.n_sources = 2;
    in.orient = HDRPLAY_SPLIT_TB;
    layout_plan(&in, &pl);
    /* Half-height pane, 1920x540: a 16:9 image fits as 960x540, so this
     * one letterboxes horizontally instead. */
    CHECK(rect_eq(pl.pass[0].target_crop, 480, 0, 1440, WIN_H / 2),
          "TB: pane A is letterboxed inside the top half");
    CHECK(rect_eq(pl.pass[1].target_crop, 480, WIN_H / 2, 1440, WIN_H),
          "TB: pane B is letterboxed inside the bottom half");

    /* SDR applies to BOTH panes — the split is spent on content, so the
     * treatment cannot also vary across it. */
    in = base_input();
    in.n_sources = 2;
    in.mode = HDRPLAY_MODE_SDR;
    layout_plan(&in, &pl);
    CHECK(pl.n_inter == 2, "AB SDR: one intermediate per pane (got %d)", pl.n_inter);
    CHECK(pl.inter[0].sdr && pl.inter[1].sdr, "AB SDR: both are SDR");
    CHECK(pl.inter[0].src != pl.inter[1].src,
          "AB SDR: intermediates come from different sources");
}

static void test_pair_diag(void)
{
    puts("two sources, DIAG wipe");
    LayoutPlan pl;
    LayoutInput in = base_input();
    in.n_sources = 2;
    in.orient = HDRPLAY_SPLIT_DIAG;
    layout_plan(&in, &pl);

    CHECK(pl.n_pass == 1, "DIAG: single pass (a wipe is not a crop)");
    CHECK(pl.pass[0].src == 0, "DIAG: A renders underneath");
    CHECK(pl.n_inter == 1 && pl.inter[0].src == 1 &&
          pl.inter[0].mask == ALPHA_MASK_DIAG,
          "DIAG HDR: B goes through a diagonal-masked intermediate");
    CHECK(!pl.inter[0].sdr, "DIAG HDR: intermediate uses HDR treatment");

    in.mode = HDRPLAY_MODE_SDR;
    layout_plan(&in, &pl);
    CHECK(pl.n_inter == 2, "DIAG SDR: A also needs an intermediate");
    CHECK(pl.inter[0].mask == ALPHA_MASK_FULL && pl.inter[0].src == 0,
          "DIAG SDR: A gets a FULL mask underneath");
    CHECK(pl.inter[1].mask == ALPHA_MASK_DIAG && pl.inter[1].src == 1,
          "DIAG SDR: B keeps the diagonal mask on top");
}

/* No overlay may be attached to more than one pass. */
static void test_overlay_routing_is_exclusive(void)
{
    puts("every overlay lands in exactly one pass");
    LayoutOverlayKind kinds[] = {
        LAYOUT_OV_STATUS, LAYOUT_OV_SESSION,
        LAYOUT_OV_LABEL_A, LAYOUT_OV_LABEL_B,
    };
    HdrplaySplitOrient orients[] = {
        HDRPLAY_SPLIT_LR, HDRPLAY_SPLIT_TB, HDRPLAY_SPLIT_DIAG,
    };
    for (int o = 0; o < 3; o++) {
        for (int m = 0; m < 3; m++) {
            LayoutInput in = base_input();
            in.n_sources = 2;
            in.session_panel = true;
            in.orient = orients[o];
            in.mode = (HdrplayMode)m;
            LayoutPlan pl; layout_plan(&in, &pl);
            for (int k = 0; k < 4; k++) {
                int n = count_ov_plan(&pl, kinds[k]);
                CHECK(n <= 1, "orient %d mode %d: overlay %d appears %dx",
                      o, m, kinds[k], n);
            }
        }
    }

    /* Under LR the session panel must sit in pane A, not straddle the
     * seam into B. */
    LayoutInput in = base_input();
    in.n_sources = 2; in.session_panel = true;
    LayoutPlan pl; layout_plan(&in, &pl);
    CHECK(count_ov(&pl.pass[0], LAYOUT_OV_SESSION) == 1,
          "LR: session panel moves to pane A (bottom-left)");
}

/* ------------------------------------------------------------------ */
static void test_zoom_pan(void)
{
    puts("zoom and pan");

    /* fit = untouched crop, so libplacebo behaves exactly as today. */
    LayoutRect r = layout_image_crop(3840, 2160, 1920, 1080, 0.0f, 0.5f, 0.5f);
    CHECK(is_full_crop(r), "zoom 0 (fit) leaves the crop untouched");

    /* 1:1 on a 1920x1080 pane of a 4K source shows a 1920x1080 window. */
    r = layout_image_crop(3840, 2160, 1920, 1080, 1.0f, 0.5f, 0.5f);
    CHECK(rect_eq(r, 960, 540, 2880, 1620), "1:1 centred on a 4K source");

    /* Panning to a corner clamps rather than running off the edge. */
    r = layout_image_crop(3840, 2160, 1920, 1080, 1.0f, 0.0f, 0.0f);
    CHECK(rect_eq(r, 0, 0, 1920, 1080), "pan to top-left clamps at the origin");
    r = layout_image_crop(3840, 2160, 1920, 1080, 1.0f, 1.0f, 1.0f);
    CHECK(rect_eq(r, 1920, 1080, 3840, 2160), "pan to bottom-right clamps");

    /* 2:1 shows half as much source in each axis. */
    r = layout_image_crop(3840, 2160, 1920, 1080, 2.0f, 0.5f, 0.5f);
    CHECK(rect_eq(r, 1440, 810, 2400, 1350), "2:1 shows a 960x540 region");

    /* Zooming out past the source size is just fit. */
    r = layout_image_crop(1920, 1080, 1920, 1080, 0.5f, 0.5f, 0.5f);
    CHECK(is_full_crop(r), "zoom below fit collapses to fit");

    /* Both panes resolve to the same source rect — the whole point of
     * locking pan across panes. */
    LayoutInput in = base_input();
    in.n_sources = 2;
    in.zoom = 1.0f; in.pan_x = 0.25f; in.pan_y = 0.75f;
    LayoutPlan pl; layout_plan(&in, &pl);
    CHECK(memcmp(&pl.pass[0].image_crop, &pl.pass[1].image_crop,
                 sizeof(LayoutRect)) == 0,
          "both panes show the identical source region");
}

/* Two files of different resolutions is an ordinary comparison job —
 * a 1080p encode against a 4K master. Both panes must show the same
 * REGION of the scene, resolved into each source's own pixels. */
static void test_mismatched_geometry(void)
{
    puts("two sources of different resolutions");
    LayoutInput in = base_input();
    in.n_sources = 2;
    in.src_w[0] = 3840; in.src_h[0] = 2160;   /* A: 4K master   */
    in.src_w[1] = 1920; in.src_h[1] = 1080;   /* B: 1080p encode */

    CHECK(layout_reference_source(&in) == 0, "larger source is the reference");

    in.zoom = 1.0f;
    LayoutPlan pl; layout_plan(&in, &pl);
    LayoutRect ca = pl.pass[0].image_crop, cb = pl.pass[1].image_crop;

    /* Same fraction of each frame, so the crops differ in pixels by
     * exactly the resolution ratio. Sharing one rect — the bug this
     * replaced — would have given both panes A's numbers. */
    float wa = ca.x1 - ca.x0, wb = cb.x1 - cb.x0;
    CHECK(fabsf(wa / wb - 2.0f) < 0.01f,
          "crop widths differ by the 2x resolution ratio (%.0f vs %.0f)", wa, wb);
    CHECK(fabsf((ca.x0 / 3840.0f) - (cb.x0 / 1920.0f)) < 0.001f,
          "both crops start at the same fraction across the frame");

    /* Centred pan on both. */
    in.pan_x = 0.5f; in.pan_y = 0.5f;
    layout_plan(&in, &pl);
    ca = pl.pass[0].image_crop; cb = pl.pass[1].image_crop;
    CHECK(fabsf(((ca.x0 + ca.x1) / 2.0f) / 3840.0f - 0.5f) < 0.001f &&
          fabsf(((cb.x0 + cb.x1) / 2.0f) / 1920.0f - 0.5f) < 0.001f,
          "both panes centred on the same point");

    /* Reference tracks size, not index. */
    in.src_w[0] = 1280; in.src_h[0] = 720;
    CHECK(layout_reference_source(&in) == 1, "reference follows the larger file");
}

/* Rotation reaches layout only as a dimension swap — layout_plan itself
 * never learns about it. These pin that contract: the swap is correct,
 * and a rotated portrait source plans exactly like a native landscape
 * one of the same displayed shape. */
static void test_rotated_dims(void)
{
    puts("layout_rotated_dims");
    int w, h;

    layout_rotated_dims(0, 1920, 1080, &w, &h);
    CHECK(w == 1920 && h == 1080, "0° passes through (%dx%d)", w, h);
    layout_rotated_dims(180, 1920, 1080, &w, &h);
    CHECK(w == 1920 && h == 1080, "180° passes through (%dx%d)", w, h);
    layout_rotated_dims(90, 1920, 1080, &w, &h);
    CHECK(w == 1080 && h == 1920, "90° swaps (%dx%d)", w, h);
    layout_rotated_dims(270, 1920, 1080, &w, &h);
    CHECK(w == 1080 && h == 1920, "270° swaps (%dx%d)", w, h);

    /* The T key increments without wrapping until read back, so a
     * caller can legitimately pass 360 or more. */
    layout_rotated_dims(450, 1920, 1080, &w, &h);
    CHECK(w == 1080 && h == 1920, "450° normalizes to 90° (%dx%d)", w, h);
    layout_rotated_dims(-90, 1920, 1080, &w, &h);
    CHECK(w == 1080 && h == 1920, "-90° normalizes to 270° (%dx%d)", w, h);
}

/* Forward map, written out independently of the implementation: where a
 * source point lands on screen once libplacebo has rotated the image
 * clockwise. Deriving the test's expectations from the same expression
 * the code uses would only prove it is self-consistent. */
static void rotate_norm_ref(int rot, double u, double v, double *fx, double *fy)
{
    switch (rot) {
    case 90:  *fx = 1.0 - v; *fy = u;       break;   /* TL -> TR */
    case 180: *fx = 1.0 - u; *fy = 1.0 - v; break;   /* TL -> BR */
    case 270: *fx = v;       *fy = 1.0 - u; break;   /* TL -> BL */
    default:  *fx = u;       *fy = v;       break;
    }
}

static void test_unrotate_norm(void)
{
    puts("layout_unrotate_norm");

    /* Round trip: forward then back must land where it started, for
     * every rotation and a spread of off-centre points that would hide
     * a transposed axis or a missing 1-x. */
    static const double pts[][2] = {
        {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0},
        {0.5, 0.5}, {0.25, 0.75}, {0.9, 0.1},
    };
    const int rots[] = { 0, 90, 180, 270 };
    int bad = 0;
    for (int r = 0; r < 4; r++) {
        for (size_t p = 0; p < sizeof pts / sizeof pts[0]; p++) {
            double fx, fy;
            rotate_norm_ref(rots[r], pts[p][0], pts[p][1], &fx, &fy);
            layout_unrotate_norm(rots[r], &fx, &fy);
            if (fabs(fx - pts[p][0]) > 1e-9 || fabs(fy - pts[p][1]) > 1e-9)
                bad++;
        }
    }
    CHECK(bad == 0, "inverts the clockwise map at every rotation (%d bad)", bad);

    /* Direction, spelled out: at 90° CW the top-left of the frame shows
     * up in the top-RIGHT of the window, so probing the top-right must
     * report the frame's top-left. A CCW implementation passes the
     * round trip above but fails this. */
    double x = 1.0, y = 0.0;
    layout_unrotate_norm(90, &x, &y);
    CHECK(fabs(x) < 1e-9 && fabs(y) < 1e-9,
          "90°: window top-right probes the frame's top-left (%.2f,%.2f)", x, y);

    x = 0.0; y = 0.0;
    layout_unrotate_norm(90, &x, &y);
    CHECK(fabs(x) < 1e-9 && fabs(y - 1.0) < 1e-9,
          "90°: window top-left probes the frame's bottom-left (%.2f,%.2f)", x, y);

    /* 0° must be untouched, so the probe is bit-identical to today's
     * behaviour for every unrotated file. */
    x = 0.31; y = 0.87;
    layout_unrotate_norm(0, &x, &y);
    CHECK(x == 0.31 && y == 0.87, "0° leaves the coordinate alone");
}

static void test_rotated_source_plans_as_native(void)
{
    puts("a rotated source plans like a native one of the same shape");

    /* 1080x1920 portrait file rotated 90° CW == a native 1920x1080. */
    int rw, rh;
    layout_rotated_dims(90, 1080, 1920, &rw, &rh);

    LayoutInput rotated = base_input();
    rotated.zoom = 1.5f;
    rotated.src_w[0] = rw; rotated.src_h[0] = rh;

    LayoutInput native = base_input();
    native.zoom = 1.5f;
    native.src_w[0] = 1920; native.src_h[0] = 1080;

    LayoutPlan pr, pn;
    layout_plan(&rotated, &pr);
    layout_plan(&native,  &pn);

    CHECK(memcmp(&pr, &pn, sizeof pr) == 0,
          "plans are byte-identical");

    /* And the swap is what makes a rotated landscape file trip the
     * portrait heuristic that main.c uses to pick a top/bottom split. */
    int dw, dh;
    layout_rotated_dims(90, 1920, 1080, &dw, &dh);
    CHECK(dh > dw, "landscape rotated 90° reads as portrait (%dx%d)", dw, dh);
    layout_rotated_dims(0, 1920, 1080, &dw, &dh);
    CHECK(dh < dw, "the same file unrotated reads as landscape (%dx%d)", dw, dh);
}

/* The central claim: whatever the mode, split, window shape, source
 * shape or zoom, the drawn rect has the same aspect as the visible
 * source region. Rather than assert a handful of rects, sweep the whole
 * space and check the invariant — a regression anywhere in layout_plan
 * shows up here even if nobody thought to write a case for it. */
static float aspect_of(LayoutRect r)
{
    float w = r.x1 - r.x0, h = r.y1 - r.y0;
    return (h > 0.0f) ? w / h : 0.0f;
}

static void test_aspect_is_always_preserved(void)
{
    puts("aspect is preserved on every path");

    static const int dims[][2] = {
        {1920, 1080}, {3840, 2160}, {1080, 1920}, {640, 480},
        {720, 576},   {4096, 1716}, {256, 256},
    };
    static const int wins[][2] = {
        {1920, 1080}, {1000, 1000}, {3000, 800}, {700, 1400}, {321, 987},
    };
    static const float zooms[] = { 0.0f, 1.0f, 2.0f, 0.5f, 8.0f };
    const int modes[]   = { HDRPLAY_MODE_HDR, HDRPLAY_MODE_SDR, HDRPLAY_MODE_SPLIT };
    const int orients[] = { HDRPLAY_SPLIT_LR, HDRPLAY_SPLIT_TB, HDRPLAY_SPLIT_DIAG };

    int checked = 0, bad = 0;
    float worst = 0.0f;

    for (size_t da = 0; da < sizeof dims / sizeof dims[0]; da++)
    for (size_t db = 0; db < sizeof dims / sizeof dims[0]; db++)
    for (size_t wi = 0; wi < sizeof wins / sizeof wins[0]; wi++)
    for (size_t zi = 0; zi < sizeof zooms / sizeof zooms[0]; zi++)
    for (int nsrc = 1; nsrc <= 2; nsrc++)
    for (int mi = 0; mi < 3; mi++)
    for (int oi = 0; oi < 3; oi++) {
        LayoutInput in = base_input();
        in.n_sources = nsrc;
        in.mode      = modes[mi];
        in.orient    = orients[oi];
        in.win_w = wins[wi][0]; in.win_h = wins[wi][1];
        in.src_w[0] = dims[da][0]; in.src_h[0] = dims[da][1];
        in.src_w[1] = dims[db][0]; in.src_h[1] = dims[db][1];
        in.zoom = zooms[zi];

        LayoutPlan pl;
        layout_plan(&in, &pl);

        for (int p = 0; p < pl.n_pass; p++) {
            LayoutRect ic = pl.pass[p].image_crop;
            int s = pl.pass[p].src;
            /* All-zero crop means the whole frame. */
            float src_aspect = rect_is_zero_t(ic)
                ? (float)in.src_w[s] / (float)in.src_h[s]
                : aspect_of(ic);
            float tgt_aspect = aspect_of(pl.pass[p].target_crop);
            checked++;
            float err = fabsf(tgt_aspect - src_aspect) / src_aspect;
            if (err > worst) worst = err;
            if (err > 0.005f) {
                if (bad == 0)
                    printf("    first bad: %dx%d in %dx%d zoom %.1f mode %d "
                           "orient %d nsrc %d -> src %.4f tgt %.4f\n",
                           in.src_w[s], in.src_h[s], in.win_w, in.win_h,
                           in.zoom, in.mode, in.orient, nsrc,
                           src_aspect, tgt_aspect);
                bad++;
            }
        }
    }
    CHECK(bad == 0, "%d plans, worst aspect error %.4f%% (%d bad)",
          checked, worst * 100.0f, bad);

    /* And the target never escapes its pane, which is the other half of
     * "fits": an aspect-correct rect hanging off the edge of the window
     * would satisfy the check above. */
    int overflow = 0;
    for (size_t wi = 0; wi < sizeof wins / sizeof wins[0]; wi++)
    for (size_t da = 0; da < sizeof dims / sizeof dims[0]; da++)
    for (size_t zi = 0; zi < sizeof zooms / sizeof zooms[0]; zi++) {
        LayoutInput in = base_input();
        in.n_sources = 2;
        in.win_w = wins[wi][0]; in.win_h = wins[wi][1];
        in.src_w[0] = in.src_w[1] = dims[da][0];
        in.src_h[0] = in.src_h[1] = dims[da][1];
        in.zoom = zooms[zi];
        LayoutPlan pl;
        layout_plan(&in, &pl);
        for (int p = 0; p < pl.n_pass; p++) {
            LayoutRect t = pl.pass[p].target_crop;
            if (t.x0 < -0.01f || t.y0 < -0.01f ||
                t.x1 > in.win_w + 0.01f || t.y1 > in.win_h + 0.01f)
                overflow++;
        }
    }
    CHECK(overflow == 0, "no target rect escapes the window (%d)", overflow);
}

/* 1:1 has to be exactly 1:1 -- one source pixel per framebuffer pixel --
 * or it is useless for the thing it exists for, which is deciding
 * whether a compression artifact is in the file or in the resampler. */
static void test_one_to_one_is_exact(void)
{
    puts("zoom 1.0 is exactly 1:1");

    struct { int sw, sh, ww, wh; } cases[] = {
        { 3840, 2160, 1920, 1080 },   /* source larger than the window  */
        {  640,  360, 1920, 1080 },   /* smaller: used to stretch to fit */
        { 1920, 1080, 1920, 1080 },   /* exact match                     */
        { 1080, 1920,  900,  700 },   /* portrait, both axes cropped     */
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        LayoutInput in = base_input();
        in.n_sources = 1;
        in.win_w = cases[i].ww; in.win_h = cases[i].wh;
        in.src_w[0] = in.src_w[1] = cases[i].sw;
        in.src_h[0] = in.src_h[1] = cases[i].sh;
        in.zoom = 1.0f;

        LayoutPlan pl;
        layout_plan(&in, &pl);
        LayoutRect ic = pl.pass[0].image_crop;
        LayoutRect t  = pl.pass[0].target_crop;

        float cw = rect_is_zero_t(ic) ? (float)cases[i].sw : ic.x1 - ic.x0;
        float ch = rect_is_zero_t(ic) ? (float)cases[i].sh : ic.y1 - ic.y0;

        CHECK(fabsf((t.x1 - t.x0) - cw) < 0.51f &&
              fabsf((t.y1 - t.y0) - ch) < 0.51f,
              "%dx%d in %dx%d: %.0fx%.0f source px -> %.0fx%.0f screen px",
              cases[i].sw, cases[i].sh, cases[i].ww, cases[i].wh,
              cw, ch, t.x1 - t.x0, t.y1 - t.y0);
        CHECK(fabsf(pl.pass[0].scale - 1.0f) < 0.001f,
              "%dx%d: reported scale is 1.0 (%.4f)",
              cases[i].sw, cases[i].sh, pl.pass[0].scale);
    }

    /* A source smaller than the pane must be centred with borders, not
     * blown up to fill -- the old behaviour, and the reason 1:1 could
     * not actually be reached on small clips. */
    LayoutInput in = base_input();
    in.n_sources = 1;
    in.win_w = 1920; in.win_h = 1080;
    in.src_w[0] = in.src_w[1] = 640;
    in.src_h[0] = in.src_h[1] = 360;
    in.zoom = 1.0f;
    LayoutPlan pl;
    layout_plan(&in, &pl);
    CHECK(rect_eq(pl.pass[0].target_crop, 640, 360, 1280, 720),
          "a 640x360 clip at 1:1 sits centred in a 1920x1080 window");
}

/* Two letterboxed panes must meet at the seam, not sit centred in their
 * own halves with the sum of both inner margins as a gutter between
 * them. Portrait content in a left/right split is where this bites: each
 * image is much narrower than its half, so centring puts a wide black
 * band exactly between the two things being compared. */
static void test_panes_meet_at_the_seam(void)
{
    puts("split panes are justified toward the seam");

    /* 1080x1920 portrait in a 1920x1080 window: each half is 960x1080,
     * each image fits as 608x1080, leaving 352px of slack per pane. */
    LayoutInput in = base_input();
    in.n_sources = 2;
    in.orient = HDRPLAY_SPLIT_LR;
    in.src_w[0] = in.src_w[1] = 1080;
    in.src_h[0] = in.src_h[1] = 1920;
    LayoutPlan pl;
    layout_plan(&in, &pl);

    LayoutRect ta = pl.pass[0].target_crop, tb = pl.pass[1].target_crop;
    CHECK(fabsf(ta.x1 - WIN_W / 2.0f) < 0.51f,
          "LR: pane A's right edge is on the seam (%.1f)", ta.x1);
    CHECK(fabsf(tb.x0 - WIN_W / 2.0f) < 0.51f,
          "LR: pane B's left edge is on the seam (%.1f)", tb.x0);
    CHECK(fabsf((tb.x0 - ta.x1)) < 0.51f, "LR: no gutter between them");
    CHECK(ta.x0 > 300.0f && tb.x1 < WIN_W - 300.0f,
          "LR: the slack went to the outside edges (%.0f .. %.0f)",
          ta.x0, tb.x1);

    /* Swapping sides must not swap which edge is justified: the LEFT
     * pane is always right-justified, whichever file is in it. */
    in.swapped = true;
    layout_plan(&in, &pl);
    CHECK(pl.pass[0].src == 1 && pl.pass[1].src == 0, "X swapped the sources");
    CHECK(fabsf(pl.pass[0].target_crop.x1 - WIN_W / 2.0f) < 0.51f &&
          fabsf(pl.pass[1].target_crop.x0 - WIN_W / 2.0f) < 0.51f,
          "LR swapped: still justified to the seam");

    /* TB: a source wider than the very wide half-pane letterboxes
     * vertically, and the two should meet at the horizontal seam. */
    in = base_input();
    in.n_sources = 2;
    in.orient = HDRPLAY_SPLIT_TB;
    in.win_w = 800; in.win_h = 1200;      /* halves are 800x600 */
    in.src_w[0] = in.src_w[1] = 1920;     /* 16:9 -> 800x450, 150 slack */
    in.src_h[0] = in.src_h[1] = 1080;
    layout_plan(&in, &pl);
    ta = pl.pass[0].target_crop; tb = pl.pass[1].target_crop;
    CHECK(fabsf(ta.y1 - 600.0f) < 0.51f,
          "TB: pane A's bottom edge is on the seam (%.1f)", ta.y1);
    CHECK(fabsf(tb.y0 - 600.0f) < 0.51f,
          "TB: pane B's top edge is on the seam (%.1f)", tb.y0);

    /* Justification must not have broken the aspect guarantee. */
    CHECK(fabsf(((ta.x1 - ta.x0) / (ta.y1 - ta.y0)) - 16.0f / 9.0f) < 0.01f,
          "TB: aspect still correct after justifying");

    /* A single pane stays centred — there is no seam to meet. */
    in = base_input();
    in.n_sources = 1;
    in.src_w[0] = 1080; in.src_h[0] = 1920;
    layout_plan(&in, &pl);
    LayoutRect t = pl.pass[0].target_crop;
    CHECK(fabsf((t.x0 + t.x1) / 2.0f - WIN_W / 2.0f) < 0.51f,
          "single pane is still centred (%.0f..%.0f)", t.x0, t.x1);
}

int main(void)
{
    test_single_file_unchanged();
    test_solo_equals_single();
    test_pair_lr_tb();
    test_pair_diag();
    test_overlay_routing_is_exclusive();
    test_zoom_pan();
    test_mismatched_geometry();
    test_rotated_dims();
    test_unrotate_norm();
    test_rotated_source_plans_as_native();
    test_aspect_is_always_preserved();
    test_one_to_one_is_exact();
    test_panes_meet_at_the_seam();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
