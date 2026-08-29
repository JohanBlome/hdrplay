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
    CHECK(rect_eq(pl.pass[0].target_crop, 0, 0, WIN_W / 2, WIN_H),
          "LR: pane A is the left half");
    CHECK(rect_eq(pl.pass[1].target_crop, WIN_W / 2, 0, WIN_W, WIN_H),
          "LR: pane B is the right half");

    in.swapped = true;
    layout_plan(&in, &pl);
    CHECK(pl.pass[0].src == 1 && pl.pass[1].src == 0,
          "X swaps which source is on the left");

    in = base_input();
    in.n_sources = 2;
    in.orient = HDRPLAY_SPLIT_TB;
    layout_plan(&in, &pl);
    CHECK(rect_eq(pl.pass[0].target_crop, 0, 0, WIN_W, WIN_H / 2),
          "TB: pane A is the top half");
    CHECK(rect_eq(pl.pass[1].target_crop, 0, WIN_H / 2, WIN_W, WIN_H),
          "TB: pane B is the bottom half");

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

int main(void)
{
    test_single_file_unchanged();
    test_solo_equals_single();
    test_pair_lr_tb();
    test_pair_diag();
    test_overlay_routing_is_exclusive();
    test_zoom_pan();
    test_mismatched_geometry();

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
