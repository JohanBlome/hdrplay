#include "hud.h"
#include "renderer.h"
#include "log.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

/* ------------------------------------------------------------------ */
/* Minimal embedded 6x8 bitmap font.                                  */
/* Only the glyphs we actually need for the HUD strings. Unknown      */
/* chars render as a solid block — fine for an insight tool, swap in  */
/* stb_truetype if you ever care about full Unicode.                  */
/*                                                                    */
/* Each glyph: 8 rows × 6 columns, MSB-first in each byte, low bit    */
/* unused.  '*' = pixel on.                                           */
/* ------------------------------------------------------------------ */

#define FONT_W 6
#define FONT_H 8

typedef struct { char c; uint8_t row[FONT_H]; } Glyph;

#define G(ch, r0,r1,r2,r3,r4,r5,r6,r7) { ch, { r0,r1,r2,r3,r4,r5,r6,r7 } }

static const Glyph FONT[] = {
    G(' ', 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    G('.', 0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x30),
    G(':', 0x00,0x30,0x30,0x00,0x00,0x30,0x30,0x00),
    G('/', 0x04,0x08,0x08,0x10,0x10,0x20,0x20,0x40),
    G('-', 0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00),
    G('=', 0x00,0x00,0x7C,0x00,0x7C,0x00,0x00,0x00),
    G('(', 0x18,0x20,0x40,0x40,0x40,0x40,0x20,0x18),
    G(')', 0x60,0x10,0x08,0x08,0x08,0x08,0x10,0x60),
    G('0', 0x78,0x84,0x8C,0x94,0xA4,0xC4,0x84,0x78),
    G('1', 0x20,0x60,0xA0,0x20,0x20,0x20,0x20,0xF8),
    G('2', 0x78,0x84,0x04,0x08,0x10,0x20,0x40,0xFC),
    G('3', 0x78,0x84,0x04,0x38,0x04,0x04,0x84,0x78),
    G('4', 0x08,0x18,0x28,0x48,0x88,0xFC,0x08,0x08),
    G('5', 0xFC,0x80,0x80,0xF8,0x04,0x04,0x84,0x78),
    G('6', 0x38,0x40,0x80,0xF8,0x84,0x84,0x84,0x78),
    G('7', 0xFC,0x04,0x08,0x10,0x20,0x40,0x40,0x40),
    G('8', 0x78,0x84,0x84,0x78,0x84,0x84,0x84,0x78),
    G('9', 0x78,0x84,0x84,0x84,0x7C,0x04,0x08,0x70),
    G('A', 0x78,0x84,0x84,0x84,0xFC,0x84,0x84,0x84),
    G('B', 0xF8,0x84,0x84,0xF8,0x84,0x84,0x84,0xF8),
    G('C', 0x78,0x84,0x80,0x80,0x80,0x80,0x84,0x78),
    G('D', 0xF0,0x88,0x84,0x84,0x84,0x84,0x88,0xF0),
    G('E', 0xFC,0x80,0x80,0xF0,0x80,0x80,0x80,0xFC),
    G('F', 0xFC,0x80,0x80,0xF0,0x80,0x80,0x80,0x80),
    G('G', 0x78,0x84,0x80,0x80,0x9C,0x84,0x84,0x78),
    G('H', 0x84,0x84,0x84,0xFC,0x84,0x84,0x84,0x84),
    G('I', 0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0xF8),
    G('J', 0x04,0x04,0x04,0x04,0x04,0x84,0x84,0x78),
    G('K', 0x84,0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88),
    G('L', 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0xFC),
    G('M', 0x82,0xC6,0xAA,0x92,0x82,0x82,0x82,0x82),
    G('N', 0x84,0xC4,0xA4,0x94,0x8C,0x84,0x84,0x84),
    G('O', 0x78,0x84,0x84,0x84,0x84,0x84,0x84,0x78),
    G('P', 0xF8,0x84,0x84,0xF8,0x80,0x80,0x80,0x80),
    G('Q', 0x78,0x84,0x84,0x84,0x84,0x94,0x88,0x74),
    G('R', 0xF8,0x84,0x84,0xF8,0x90,0x88,0x84,0x84),
    G('S', 0x78,0x84,0x80,0x78,0x04,0x04,0x84,0x78),
    G('T', 0xFC,0x20,0x20,0x20,0x20,0x20,0x20,0x20),
    G('U', 0x84,0x84,0x84,0x84,0x84,0x84,0x84,0x78),
    G('V', 0x84,0x84,0x84,0x84,0x84,0x48,0x48,0x30),
    G('W', 0x82,0x82,0x82,0x82,0x92,0xAA,0xC6,0x82),
    G('X', 0x84,0x84,0x48,0x30,0x30,0x48,0x84,0x84),
    G('Y', 0x84,0x84,0x84,0x48,0x30,0x20,0x20,0x20),
    G('Z', 0xFC,0x04,0x08,0x10,0x20,0x40,0x80,0xFC),
};

static const Glyph *find_glyph(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    for (size_t i = 0; i < sizeof(FONT)/sizeof(FONT[0]); i++)
        if (FONT[i].c == c) return &FONT[i];
    return NULL;
}

/* Render a single line of text into an RGBA8 buffer at (x_px, y_px),
 * scaled by `scale`, in the given RGB color. */
static void draw_text_color(uint8_t *rgba, int W, int H,
                            int x_px, int y_px, int scale, const char *s,
                            uint8_t r, uint8_t g, uint8_t b)
{
    for (; *s; s++) {
        const Glyph *g_ = find_glyph(*s);
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = g_ ? g_->row[row] : 0x7E;
            for (int col = 0; col < FONT_W; col++) {
                bool on = (bits >> (7 - col)) & 1;
                if (!on) continue;
                for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++) {
                    int px = x_px + col * scale + dx;
                    int py = y_px + row * scale + dy;
                    if (px < 0 || px >= W || py < 0 || py >= H) continue;
                    uint8_t *p = &rgba[(py * W + px) * 4];
                    p[0] = r; p[1] = g; p[2] = b;
                    p[3] = 255;
                }
            }
        }
        x_px += (FONT_W + 1) * scale;
    }
}

/* Backwards-compat: default to white text. */
static void draw_text(uint8_t *rgba, int W, int H,
                      int x_px, int y_px, int scale, const char *s)
{
    draw_text_color(rgba, W, H, x_px, y_px, scale, s, 255, 255, 255);
}

/* HUD slots:
 *  0 = status panel  (top-left, multi-line)
 *  1 = HDR badge     (split mode only — half-1 label)
 *  2 = SDR badge     (split mode only — half-2 label)
 *
 * Each slot owns its own pl_tex so they can be sized independently and
 * positioned anywhere on the swapchain image via the overlay's dst rect. */
enum { SLOT_STATUS, SLOT_HDR_LABEL, SLOT_SDR_LABEL, SLOT_COUNT };

typedef struct { pl_tex tex; int W, H; } HudSlot;

static HudSlot      slots[SLOT_COUNT];
static int          hud_scale = 2;
static uint64_t     frame_count = 0;

/* Storage for overlay descriptors passed to libplacebo each frame.
 * Sized once, mutated per-frame. */
static struct pl_overlay      overlay_arr[SLOT_COUNT];
static struct pl_overlay_part overlay_parts[SLOT_COUNT];

static void ensure_slot(int s, pl_gpu gpu, int W, int H)
{
    if (slots[s].tex && slots[s].W == W && slots[s].H == H) return;
    if (slots[s].tex) pl_tex_destroy(gpu, &slots[s].tex);
    slots[s].W = W; slots[s].H = H;
    slots[s].tex = pl_tex_create(gpu, pl_tex_params(
        .w              = W,
        .h              = H,
        .format         = pl_find_named_fmt(gpu, "rgba8"),
        .sampleable     = true,
        .host_writable  = true,
    ));
}

/* Common: paint a translucent black background panel and return the
 * draw buffer. Caller owns the buffer and must free() it. */
static uint8_t *make_panel(int W, int H, uint8_t alpha)
{
    uint8_t *buf = calloc(W * H * 4, 1);
    if (!buf) return NULL;
    for (int i = 0; i < W * H; i++) {
        buf[i*4 + 0] = 0;
        buf[i*4 + 1] = 0;
        buf[i*4 + 2] = 0;
        buf[i*4 + 3] = alpha;
    }
    return buf;
}

/* Upload `buf` into a slot's texture and configure overlay_arr[s] +
 * overlay_parts[s] so that the slot renders at the given dst rect. */
static void commit_slot(int s, pl_gpu gpu, uint8_t *buf,
                        int dst_x, int dst_y)
{
    pl_tex_upload(gpu, pl_tex_transfer_params(
        .tex = slots[s].tex,
        .ptr = buf,
    ));
    overlay_parts[s] = (struct pl_overlay_part){
        .src = { 0, 0, slots[s].W, slots[s].H },
        .dst = { dst_x, dst_y, dst_x + slots[s].W, dst_y + slots[s].H },
    };
    overlay_arr[s] = (struct pl_overlay){
        .tex      = slots[s].tex,
        .mode     = PL_OVERLAY_NORMAL,
        .parts    = &overlay_parts[s],
        .num_parts = 1,
        .repr     = pl_color_repr_rgb,
        .color    = pl_color_space_srgb,
    };
}

/* Build the multi-line status panel at top-left. */
static int build_status_panel(Renderer *r, pl_gpu gpu, int win_w, int win_h)
{
    (void)win_w; (void)win_h;
    const int W = 460, H = 220;
    ensure_slot(SLOT_STATUS, gpu, W, H);
    if (!slots[SLOT_STATUS].tex) return -1;
    uint8_t *buf = make_panel(W, H, 170);
    if (!buf) return -1;

    char line[160];
    int y = 6;

    const char *mode_str =
        r->mode == HDRPLAY_MODE_HDR ? "HDR" :
        r->mode == HDRPLAY_MODE_SDR ? "SDR" :
        r->mode == HDRPLAY_MODE_SPLIT
            ? (r->split_orient == HDRPLAY_SPLIT_TB   ? "SPLIT TB" :
               r->split_orient == HDRPLAY_SPLIT_DIAG ? "SPLIT DIAG" :
                                                       "SPLIT LR")
            : "?";
    snprintf(line, sizeof(line), "MODE %s", mode_str);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += FONT_H * hud_scale + 2;

    /* Headroom line — turns red when so high that dark content disappears.
     * Threshold 8x roughly equals SDR-white ≈ 25 nits, below which
     * 0-10 nit content becomes hard to see in a normally-lit room. */
    bool excessive_headroom = r->display_hdr_headroom > 8.0f;
    snprintf(line, sizeof(line), "DISPLAY HDR %s HEADROOM %.2fX",
             r->display_hdr_capable ? "ON" : "OFF", r->display_hdr_headroom);
    if (excessive_headroom)
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 80, 80);
    else
        draw_text(buf, W, H, 6, y, hud_scale, line);
    y += FONT_H * hud_scale + 2;

    if (excessive_headroom) {
        draw_text_color(buf, W, H, 6, y, hud_scale,
                        "DARK CONTENT MAY BE INVISIBLE", 255, 80, 80);
        y += FONT_H * hud_scale + 2;
    }

    /* Source colorspace, compact form. */
    if (r->last_source_csp[0])
        draw_text(buf, W, H, 6, y, hud_scale, r->last_source_csp);
    y += FONT_H * hud_scale + 2;

    /* Output target, compact form. */
    if (r->last_output_csp[0])
        draw_text(buf, W, H, 6, y, hud_scale, r->last_output_csp);
    y += FONT_H * hud_scale + 2;

    snprintf(line, sizeof(line), "FRAME %llu%s%s",
             (unsigned long long)frame_count,
             r->paused       ? " PAUSED" : "",
             r->loop_enabled ? " LOOP"   : "");
    draw_text(buf, W, H, 6, y, hud_scale, line); y += FONT_H * hud_scale + 2;

    /* Luminance probe — reports nominal nits of the source pixel under
     * the cursor. Greenish so it stands out from the rest of the panel. */
    if (r->probe_active) {
        snprintf(line, sizeof(line), "PROBE (%d,%d) %.1f NITS",
                 r->probe_x, r->probe_y, r->probe_nits);
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 120, 255, 120);
        y += FONT_H * hud_scale + 2;
        snprintf(line, sizeof(line), "R %.1f G %.1f B %.1f",
                 r->probe_r_nits, r->probe_g_nits, r->probe_b_nits);
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 120, 255, 120);
        y += FONT_H * hud_scale + 2;
    }

    commit_slot(SLOT_STATUS, gpu, buf, 16, 16);
    free(buf);
    return 0;
}

/* Build a single big "HDR" or "SDR" badge with a subtitle. Used per-half
 * in split mode so you can instantly tell which side is which. */
static int build_label_badge(int slot, pl_gpu gpu, const char *big,
                              const char *sub, int dst_x, int dst_y)
{
    const int W = 200, H = 64;
    ensure_slot(slot, gpu, W, H);
    if (!slots[slot].tex) return -1;
    uint8_t *buf = make_panel(W, H, 200);
    if (!buf) return -1;

    /* Big label at top in 4x scale (so it reads from across the room). */
    draw_text(buf, W, H, 12, 8, 4, big);
    /* Subtitle in normal scale beneath. */
    if (sub) draw_text(buf, W, H, 12, 8 + FONT_H * 4 + 4, hud_scale, sub);

    commit_slot(slot, gpu, buf, dst_x, dst_y);
    free(buf);
    return 0;
}

/* Public entry point. Builds whichever overlay textures the current
 * mode needs and writes them back through `out`. The caller decides
 * which overlays to attach to which pl_render_image pass:
 *
 *   HDR / SDR mode  → attach `out->status` (1 overlay) to the single render.
 *   SPLIT mode      → attach status + hdr_label to the FIRST (HDR-half) render;
 *                     attach sdr_label to the SECOND (SDR-half) render.
 *                     This is required because overlays are clipped to the
 *                     render's target.crop — a status panel in the left half
 *                     would be invisible if attached only to the right-half
 *                     render. */
void hud_prepare(Renderer *r, int win_w, int win_h, HudOverlays *out)
{
    frame_count++;
    pl_gpu gpu = r->vulkan->gpu;

    memset(out, 0, sizeof(*out));
    out->win_w = win_w;
    out->win_h = win_h;

    build_status_panel(r, gpu, win_w, win_h);
    out->status = overlay_arr[SLOT_STATUS];

    if (r->mode == HDRPLAY_MODE_SPLIT) {
        const int label_w = 200, label_h = 64;
        int hdr_x, hdr_y, sdr_x, sdr_y;
        switch (r->split_orient) {
        case HDRPLAY_SPLIT_LR:
            hdr_x = win_w / 2 - label_w - 16;  hdr_y = 16;
            sdr_x = win_w / 2 + 16;            sdr_y = 16;
            break;
        case HDRPLAY_SPLIT_TB:
            hdr_x = 16;  hdr_y = win_h / 2 - label_h - 16;
            sdr_x = 16;  sdr_y = win_h / 2 + 16;
            break;
        case HDRPLAY_SPLIT_DIAG:
        default:
            /* HDR triangle in upper-left → badge near top-left corner.
             * SDR triangle in lower-right → badge near bottom-right corner.
             * Y offsets put them away from the diagonal seam. */
            hdr_x = 16;                      hdr_y = win_h - 16 - label_h * 3;
            sdr_x = win_w - 16 - label_w;    sdr_y = 16 + label_h * 3;
            break;
        }
        char hdr_sub[64], sdr_sub[64];
        snprintf(hdr_sub, sizeof(hdr_sub), "BT.2020 PQ %.0fNITS",
                 203.0f * r->display_hdr_headroom);
        snprintf(sdr_sub, sizeof(sdr_sub), "TONEMAP %.0fNITS",
                 r->sdr_peak_effective);
        build_label_badge(SLOT_HDR_LABEL, gpu, "HDR", hdr_sub, hdr_x, hdr_y);
        build_label_badge(SLOT_SDR_LABEL, gpu, "SDR", sdr_sub, sdr_x, sdr_y);
        out->hdr_label = overlay_arr[SLOT_HDR_LABEL];
        out->sdr_label = overlay_arr[SLOT_SDR_LABEL];
    }
}
