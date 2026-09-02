#include "layout.h"

#include <string.h>

/* Panel geometry, mirrored from hud.c. Layout needs the rects to decide
 * which pass each panel belongs to; hud.c owns the drawing. */
#define STATUS_W  460
#define STATUS_H  364
#define SESSION_W 440
#define SESSION_H 200
#define LABEL_W   200
#define LABEL_H    64
#define MARGIN     16

static LayoutRect rect(float x0, float y0, float x1, float y1)
{
    return (LayoutRect){ x0, y0, x1, y1 };
}

static bool rect_contains(LayoutRect outer, LayoutRect inner)
{
    return inner.x0 >= outer.x0 && inner.y0 >= outer.y0 &&
           inner.x1 <= outer.x1 && inner.y1 <= outer.y1;
}

static void add_ov(LayoutPass *p, LayoutOverlayKind kind, int src, LayoutRect dst)
{
    if (p->n_ov >= LAYOUT_MAX_OVERLAYS) return;
    p->ov[p->n_ov].kind = kind;
    p->ov[p->n_ov].src  = src;
    p->ov[p->n_ov].dst  = dst;
    p->n_ov++;
}

/* ------------------------------------------------------------------ */
int layout_reference_source(const LayoutInput *in)
{
    if (in->n_sources < 2) return 0;
    long a = (long)in->src_w[0] * in->src_h[0];
    long b = (long)in->src_w[1] * in->src_h[1];
    return (b > a) ? 1 : 0;
}

void layout_rotated_dims(int rot, int w, int h, int *out_w, int *out_h)
{
    /* Normalize first: the CLI clamps to {0,90,180,270}, but the T key
     * increments without wrapping until it is read back, and a future
     * display-matrix seed could arrive negative. */
    int r = ((rot % 360) + 360) % 360;
    if (r == 90 || r == 270) { *out_w = h; *out_h = w; }
    else                     { *out_w = w; *out_h = h; }
}

void layout_unrotate_norm(int rot, double *x, double *y)
{
    double fx = *x, fy = *y;
    switch (((rot % 360) + 360) % 360) {
    case 90:  *x = fy;        *y = 1.0 - fx;  break;
    case 180: *x = 1.0 - fx;  *y = 1.0 - fy;  break;
    case 270: *x = 1.0 - fy;  *y = fx;        break;
    default:  break;
    }
}

float layout_fit_pane(int src_w, int src_h, int ref_w, int ref_h,
                      LayoutRect pane, float zoom, float pan_x, float pan_y,
                      float align_x, float align_y,
                      LayoutRect *out_image, LayoutRect *out_target)
{
    float pane_w = pane.x1 - pane.x0;
    float pane_h = pane.y1 - pane.y0;

    if (src_w <= 0 || src_h <= 0 || pane_w <= 0.0f || pane_h <= 0.0f) {
        *out_image  = rect(0, 0, 0, 0);
        *out_target = pane;
        return 1.0f;
    }
    if (ref_w <= 0 || ref_h <= 0) { ref_w = src_w; ref_h = src_h; }

    /* One number does all the work: screen pixels per source pixel.
     *
     * Fit takes the larger scale that still gets the whole frame inside
     * the pane. Zoom is expressed against the REFERENCE source, not this
     * one, so that two files of different resolutions cover the same
     * region of the scene rather than the same pixel count — at 1:1 a
     * 1080p pane would otherwise show four times the area of a 4K one,
     * which makes the panes uncomparable exactly when you have zoomed in
     * to compare them. */
    float scale;
    if (zoom > 0.0f) {
        scale = zoom * (float)ref_w / (float)src_w;
    } else {
        float sx = pane_w / (float)src_w;
        float sy = pane_h / (float)src_h;
        scale = sx < sy ? sx : sy;
    }
    if (!(scale > 0.0f)) scale = 1.0f;

    /* Visible source region: as much as the pane holds at that scale,
     * never more than exists. Clamping here rather than after scaling is
     * what makes the target below fit the pane by construction, with no
     * second clamp that could disagree with this one. */
    float vis_w = pane_w / scale;
    float vis_h = pane_h / scale;
    if (vis_w > (float)src_w) vis_w = (float)src_w;
    if (vis_h > (float)src_h) vis_h = (float)src_h;

    float x0 = pan_x * (float)src_w - vis_w * 0.5f;
    float y0 = pan_y * (float)src_h - vis_h * 0.5f;
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    if (x0 + vis_w > (float)src_w) x0 = (float)src_w - vis_w;
    if (y0 + vis_h > (float)src_h) y0 = (float)src_h - vis_h;

    /* The whole frame keeps the all-zero convention, so pl_frame.crop is
     * left untouched exactly as before. */
    if (vis_w >= (float)src_w && vis_h >= (float)src_h)
        *out_image = rect(0, 0, 0, 0);
    else
        *out_image = rect(x0, y0, x0 + vis_w, y0 + vis_h);

    /* Target aspect == crop aspect, always: both are the same rect times
     * one scalar. That identity IS the aspect guarantee — there is no
     * separate letterbox step that could be skipped on some path. */
    float tw = vis_w * scale;
    float th = vis_h * scale;

    /* Where the slack goes. 0.5 centres; a two-pane split pushes each
     * image toward the seam so the two meet in the middle instead of
     * being separated by the sum of their inner margins. */
    if (align_x < 0.0f) align_x = 0.0f;
    if (align_x > 1.0f) align_x = 1.0f;
    if (align_y < 0.0f) align_y = 0.0f;
    if (align_y > 1.0f) align_y = 1.0f;
    float ox = (pane_w - tw) * align_x;
    float oy = (pane_h - th) * align_y;
    *out_target = rect(pane.x0 + ox,      pane.y0 + oy,
                       pane.x0 + ox + tw, pane.y0 + oy + th);
    return scale;
}

LayoutRect layout_image_crop_ref(int src_w, int src_h,
                                 int ref_w, int ref_h,
                                 int dst_w, int dst_h,
                                 float zoom, float pan_x, float pan_y)
{
    LayoutRect img, tgt;
    layout_fit_pane(src_w, src_h, ref_w, ref_h,
                    rect(0, 0, (float)dst_w, (float)dst_h),
                    zoom, pan_x, pan_y, 0.5f, 0.5f, &img, &tgt);
    return img;
}

LayoutRect layout_image_crop(int src_w, int src_h, int dst_w, int dst_h,
                             float zoom, float pan_x, float pan_y)
{
    return layout_image_crop_ref(src_w, src_h, src_w, src_h,
                                 dst_w, dst_h, zoom, pan_x, pan_y);
}

/* ------------------------------------------------------------------ */
/* Where each HUD panel sits, and therefore which pass owns it.        */
/*                                                                     */
/* An overlay must be attached to exactly one pass. Attaching the same  */
/* one to two passes composites it twice — at alpha 170 the background  */
/* would blend to ~0.89 instead of 0.667, visibly darker than every     */
/* other panel. So panels are placed to fall wholly inside one pane,    */
/* never straddling the seam.                                          */
static LayoutRect status_rect(void)
{
    return rect(MARGIN, MARGIN, MARGIN + STATUS_W, MARGIN + STATUS_H);
}

static LayoutRect session_rect(const LayoutInput *in, bool two_pane)
{
    /* Single pane: bottom-right, the only corner nothing else uses.
     * Two panes under LR: bottom-LEFT instead, so it stays inside pane
     * A rather than landing in B's half. Under TB the bottom half is
     * pane B, so bottom-right is already unambiguous. */
    if (two_pane && in->orient == HDRPLAY_SPLIT_LR)
        return rect(MARGIN, in->win_h - MARGIN - SESSION_H,
                    MARGIN + SESSION_W, in->win_h - MARGIN);
    return rect(in->win_w - MARGIN - SESSION_W, in->win_h - MARGIN - SESSION_H,
                in->win_w - MARGIN, in->win_h - MARGIN);
}

/* Badge positions, matching hud.c's existing placement. */
static void label_rects(const LayoutInput *in, LayoutRect *a, LayoutRect *b)
{
    switch (in->orient) {
    case HDRPLAY_SPLIT_TB:
        *a = rect(MARGIN, in->win_h / 2.0f - LABEL_H - MARGIN,
                  MARGIN + LABEL_W, in->win_h / 2.0f - MARGIN);
        *b = rect(MARGIN, in->win_h / 2.0f + MARGIN,
                  MARGIN + LABEL_W, in->win_h / 2.0f + MARGIN + LABEL_H);
        break;
    case HDRPLAY_SPLIT_DIAG:
        *a = rect(MARGIN, in->win_h - MARGIN - LABEL_H * 3.0f,
                  MARGIN + LABEL_W, in->win_h - MARGIN - LABEL_H * 2.0f);
        *b = rect(in->win_w - MARGIN - LABEL_W, MARGIN + LABEL_H * 3.0f,
                  in->win_w - MARGIN, MARGIN + LABEL_H * 4.0f);
        break;
    case HDRPLAY_SPLIT_LR:
    default:
        *a = rect(in->win_w / 2.0f - LABEL_W - MARGIN, MARGIN,
                  in->win_w / 2.0f - MARGIN, MARGIN + LABEL_H);
        *b = rect(in->win_w / 2.0f + MARGIN, MARGIN,
                  in->win_w / 2.0f + MARGIN + LABEL_W, MARGIN + LABEL_H);
        break;
    }
}

static int mask_for_orient(HdrplaySplitOrient o)
{
    switch (o) {
    case HDRPLAY_SPLIT_TB:   return ALPHA_MASK_TB;
    case HDRPLAY_SPLIT_DIAG: return ALPHA_MASK_DIAG;
    case HDRPLAY_SPLIT_LR:
    default:                 return ALPHA_MASK_LR;
    }
}

/* ------------------------------------------------------------------ */
/* One source. This reproduces the pre-existing behaviour exactly and   */
/* must keep doing so — see the regression test.                        */
/*                                                                      */
/*   HDR   : one pass, full crop, no intermediate.                      */
/*   SDR   : intermediate with a FULL alpha mask, composited over an     */
/*           HDR base render of the same frame.                         */
/*   SPLIT : same, with the orientation's mask so only half shows.       */
/*                                                                      */
/* SDR goes through an intermediate rather than rendering the source     */
/* with an SDR target because libplacebo's overlay compositor blends in  */
/* the overlay's declared colour space WITHOUT tone-mapping, preserving  */
/* absolute PQ brightness. Rendering the intermediate as a source would  */
/* renormalize it back to panel peak. See RENDERING.md §6.6.             */
static void plan_single(const LayoutInput *in, LayoutPlan *out, int src)
{
    int ref = layout_reference_source(in);
    LayoutRect full = rect(0, 0, (float)in->win_w, (float)in->win_h);
    LayoutRect img, tgt;
    float scale = layout_fit_pane(in->src_w[src], in->src_h[src],
                                  in->src_w[ref], in->src_h[ref],
                                  full, in->zoom, in->pan_x, in->pan_y,
                                  0.5f, 0.5f, &img, &tgt);

    LayoutPass *p = &out->pass[0];
    out->n_pass   = 1;
    p->src         = src;
    p->target_crop = tgt;
    p->image_crop  = img;
    p->scale       = scale;

    if (in->mode != HDRPLAY_MODE_HDR) {
        out->inter[0] = (LayoutInter){
            .src  = src,
            .mask = (in->mode == HDRPLAY_MODE_SDR)
                    ? ALPHA_MASK_FULL : mask_for_orient(in->orient),
            .sdr  = true,
            .dst  = tgt,
            .image_crop = img,
        };
        out->n_inter = 1;
        /* First, so HUD text drawn afterwards is not erased by it: the
         * intermediate is opaque in its visible region. */
        add_ov(p, LAYOUT_OV_INTERMEDIATE, src, full);
    }

    if (!in->hud_hidden)
        add_ov(p, LAYOUT_OV_STATUS, -1, status_rect());

    if (in->mode == HDRPLAY_MODE_SPLIT) {
        LayoutRect la, lb;
        label_rects(in, &la, &lb);
        add_ov(p, LAYOUT_OV_LABEL_A, -1, la);
        add_ov(p, LAYOUT_OV_LABEL_B, -1, lb);
    }

    if (in->session_panel)
        add_ov(p, LAYOUT_OV_SESSION, -1, session_rect(in, false));

    out->name = in->mode == HDRPLAY_MODE_HDR ? "HDR" :
                in->mode == HDRPLAY_MODE_SDR ? "SDR" :
                in->orient == HDRPLAY_SPLIT_LR ? "SPLIT-LR" :
                in->orient == HDRPLAY_SPLIT_TB ? "SPLIT-TB" : "SPLIT-DIAG";
}

/* ------------------------------------------------------------------ */
/* Two sources: the split is the LAYOUT, so both panes get the same     */
/* treatment and H/S switch them together. Comparing content and        */
/* treatment at once would leave any observed difference with two        */
/* possible causes.                                                     */
/*                                                                      */
/* LR and TB use two passes with rectangular crops — direct, no          */
/* round-trip, and it avoids hand-matching the overlay compositor's      */
/* colour-space metadata. DIAG cannot be expressed as a crop, so it      */
/* keeps the alpha-mask overlay path, with source B as the masked        */
/* second image instead of the SDR version of A.                        */
static void plan_pair(const LayoutInput *in, LayoutPlan *out)
{
    int a = in->swapped ? 1 : 0;
    int b = in->swapped ? 0 : 1;
    bool sdr = (in->mode == HDRPLAY_MODE_SDR);

    LayoutRect full = rect(0, 0, (float)in->win_w, (float)in->win_h);
    LayoutRect la, lb;
    label_rects(in, &la, &lb);

    if (in->orient == HDRPLAY_SPLIT_DIAG) {
        /* Single pass: A underneath, B composited through the diagonal
         * mask. When SDR is active both need the SDR treatment, so A
         * gets a FULL-mask intermediate of its own. */
        out->n_pass = 1;
        LayoutPass *p = &out->pass[0];
        int ref = layout_reference_source(in);
        LayoutRect ia, ta, ib_, tb;
        p->src   = a;
        p->scale = layout_fit_pane(in->src_w[a], in->src_h[a],
                                   in->src_w[ref], in->src_h[ref],
                                   full, in->zoom, in->pan_x, in->pan_y,
                                   0.5f, 0.5f, &ia, &ta);
        p->target_crop = ta;
        p->image_crop  = ia;

        /* B fills the same pane, so it is fitted independently — two
         * files of different aspect each keep their own. */
        layout_fit_pane(in->src_w[b], in->src_h[b],
                        in->src_w[ref], in->src_h[ref],
                        full, in->zoom, in->pan_x, in->pan_y,
                        0.5f, 0.5f, &ib_, &tb);

        if (sdr) {
            out->inter[out->n_inter++] = (LayoutInter){
                .src = a, .mask = ALPHA_MASK_FULL, .sdr = true,
                .dst = ta, .image_crop = ia };
            add_ov(p, LAYOUT_OV_INTERMEDIATE, a, full);
        }
        out->inter[out->n_inter++] = (LayoutInter){
            .src = b, .mask = ALPHA_MASK_DIAG, .sdr = sdr,
            .dst = tb, .image_crop = ib_ };
        add_ov(p, LAYOUT_OV_INTERMEDIATE, b, full);

        if (!in->hud_hidden)   add_ov(p, LAYOUT_OV_STATUS, -1, status_rect());
        add_ov(p, LAYOUT_OV_LABEL_A, -1, la);
        add_ov(p, LAYOUT_OV_LABEL_B, -1, lb);
        if (in->session_panel)
            add_ov(p, LAYOUT_OV_SESSION, -1, session_rect(in, true));

        out->name = "AB-DIAG";
        return;
    }

    /* LR / TB: two passes, one per half. */
    LayoutRect ca, cb;
    if (in->orient == HDRPLAY_SPLIT_TB) {
        ca = rect(0, 0, (float)in->win_w, in->win_h / 2.0f);
        cb = rect(0, in->win_h / 2.0f, (float)in->win_w, (float)in->win_h);
        out->name = "AB-TB";
    } else {
        ca = rect(0, 0, in->win_w / 2.0f, (float)in->win_h);
        cb = rect(in->win_w / 2.0f, 0, (float)in->win_w, (float)in->win_h);
        out->name = "AB-LR";
    }

    int ref = layout_reference_source(in);

    /* One fit per source: the same FRACTION of each frame, resolved into
     * that source's own pixels, and its own letterbox inside its own
     * half. Sharing a single rect would express pane B's crop in pane
     * A's pixel space, which is silently wrong the moment the two files
     * differ in resolution — or, now, in aspect. */
    /* Justify toward the seam: under LR pane A goes hard right and B
     * hard left, so the two images touch in the middle. Centring each in
     * its own half instead separates them by the sum of the two inner
     * margins, which for portrait content in a left/right split is a
     * wide black gutter down the centre — exactly between the two things
     * you are trying to compare. */
    float ax = 0.5f, ay = 0.5f, bx = 0.5f, by = 0.5f;
    if (in->orient == HDRPLAY_SPLIT_TB) { ay = 1.0f; by = 0.0f; }
    else                                { ax = 1.0f; bx = 0.0f; }

    LayoutRect ia, ta, ib_, tb;
    float sa_ = layout_fit_pane(in->src_w[a], in->src_h[a],
                                in->src_w[ref], in->src_h[ref],
                                ca, in->zoom, in->pan_x, in->pan_y,
                                ax, ay, &ia, &ta);
    float sb_ = layout_fit_pane(in->src_w[b], in->src_h[b],
                                in->src_w[ref], in->src_h[ref],
                                cb, in->zoom, in->pan_x, in->pan_y,
                                bx, by, &ib_, &tb);

    out->n_pass = 2;
    LayoutPass *pa = &out->pass[0];
    LayoutPass *pb = &out->pass[1];
    pa->src = a; pa->target_crop = ta; pa->image_crop = ia; pa->scale = sa_;
    pb->src = b; pb->target_crop = tb; pb->image_crop = ib_; pb->scale = sb_;

    if (sdr) {
        out->inter[out->n_inter++] = (LayoutInter){
            .src = a, .mask = ALPHA_MASK_FULL, .sdr = true,
            .dst = ta, .image_crop = ia };
        out->inter[out->n_inter++] = (LayoutInter){
            .src = b, .mask = ALPHA_MASK_FULL, .sdr = true,
            .dst = tb, .image_crop = ib_ };
        /* Whole window for both: each intermediate is window-sized and
         * already positioned, with alpha 0 outside its own pane. */
        add_ov(pa, LAYOUT_OV_INTERMEDIATE, a, full);
        add_ov(pb, LAYOUT_OV_INTERMEDIATE, b, full);
    }

    /* Route each panel to the pass whose crop contains it. Panels are
     * positioned so this is unambiguous; the containment test is here
     * so a future move cannot silently put one in the wrong pass. */
    LayoutRect sr = status_rect();
    if (!in->hud_hidden)
        add_ov(rect_contains(ca, sr) ? pa : pb, LAYOUT_OV_STATUS, -1, sr);

    add_ov(rect_contains(ca, la) ? pa : pb, LAYOUT_OV_LABEL_A, -1, la);
    add_ov(rect_contains(ca, lb) ? pa : pb, LAYOUT_OV_LABEL_B, -1, lb);

    if (in->session_panel) {
        LayoutRect qr = session_rect(in, true);
        add_ov(rect_contains(ca, qr) ? pa : pb, LAYOUT_OV_SESSION, -1, qr);
    }
}

/* ------------------------------------------------------------------ */
void layout_plan(const LayoutInput *in, LayoutPlan *out)
{
    memset(out, 0, sizeof(*out));

    /* Solo, or only one file open, is the single-source path verbatim —
     * including the HDR-vs-SDR split, which is the whole reason solo
     * exists. */
    if (in->n_sources < 2 || in->solo >= 0) {
        int src = (in->n_sources < 2) ? 0
                : (in->solo >= 0 ? in->solo : 0);
        plan_single(in, out, src);
        return;
    }
    plan_pair(in, out);
}
