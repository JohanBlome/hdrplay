#include "hud.h"
#include "renderer.h"
#include "log.h"
#include "stats.h"
#include "source.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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
    G('+', 0x00,0x10,0x10,0x7C,0x10,0x10,0x00,0x00),
    G('<', 0x00,0x08,0x10,0x20,0x10,0x08,0x00,0x00),
    G('>', 0x00,0x20,0x10,0x08,0x10,0x20,0x00,0x00),
    G('%', 0xC4,0xC8,0x10,0x20,0x40,0x98,0x18,0x00),
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
enum {
    SLOT_STATUS,
    SLOT_HDR_LABEL,
    SLOT_SDR_LABEL,
    SLOT_PLANE_LABEL,
    SLOT_SESSION,
    SLOT_SCOPE,
    SLOT_SCOPE_ROI,
    SLOT_COUNT,
};

typedef struct { pl_tex tex; int W, H; } HudSlot;

static HudSlot      slots[SLOT_COUNT];
static int          hud_scale = 2;
static int          scope_cache_src = -1;
static int          scope_cache_frame = -1;
static int          scope_cache_w = 0, scope_cache_h = 0;
static HdrplayScopeView scope_cache_view = HDRPLAY_SCOPE_OFF;
static float        scope_cache_vector_gain = 0.0f;
static bool         scope_cache_roi_active = false;
static ProbeRegion  scope_cache_roi = {0};

/* Storage for overlay descriptors passed to libplacebo each frame.
 * Sized once, mutated per-frame. */
static struct pl_overlay      overlay_arr[SLOT_COUNT];
static struct pl_overlay_part overlay_parts[SLOT_COUNT];
static struct pl_overlay_part roi_parts[4];

void hud_close(pl_gpu gpu)
{
    for (int s = 0; s < SLOT_COUNT; s++) {
        if (slots[s].tex) pl_tex_destroy(gpu, &slots[s].tex);
        slots[s].W = slots[s].H = 0;
    }
    scope_cache_src = scope_cache_frame = -1;
    scope_cache_w = scope_cache_h = 0;
    scope_cache_view = HDRPLAY_SCOPE_OFF;
    scope_cache_vector_gain = 0.0f;
    scope_cache_roi_active = false;
    scope_cache_roi = (ProbeRegion){0};
}

static const ProbeRegion *scope_region_for(const Renderer *r, int src)
{
    return src >= 0 && src < 2 && r->scope_roi_active[src]
         ? &r->scope_roi[src] : NULL;
}

static bool scope_region_changed(const Renderer *r, int src)
{
    const ProbeRegion *roi = scope_region_for(r, src);
    if ((roi != NULL) != scope_cache_roi_active) return true;
    return roi && memcmp(roi, &scope_cache_roi, sizeof(*roi)) != 0;
}

static void scope_region_cache(const Renderer *r, int src)
{
    const ProbeRegion *roi = scope_region_for(r, src);
    scope_cache_roi_active = roi != NULL;
    scope_cache_roi = roi ? *roi : (ProbeRegion){0};
}

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
static void position_slot(int s, int dst_x, int dst_y)
{
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

static void commit_slot(int s, pl_gpu gpu, uint8_t *buf,
                        int dst_x, int dst_y)
{
    pl_tex_upload(gpu, pl_tex_transfer_params(
        .tex = slots[s].tex,
        .ptr = buf,
    ));
    position_slot(s, dst_x, dst_y);
}

static void scope_pixel(uint8_t *buf, int W, int H, int x, int y,
                        int channel, uint8_t value)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    uint8_t *p = &buf[(y * W + x) * 4];
    if (value > p[channel]) p[channel] = value;
    if (p[3] < 235) p[3] = 235;
}

/* Full-frame scopes retain their established absolute density rendering.
 * A small ROI may contain only a few hundred sparse samples, however, making
 * every occupied bin nearly black. Normalize only display intensity in that
 * case: bin positions and relative square-root density remain untouched. */
static uint8_t scope_density(uint32_t count, uint32_t peak,
                             float absolute_scale, bool normalize)
{
    if (!count) return 0;
    float value = normalize && peak
        ? 64.0f + 171.0f * sqrtf((float)count / (float)peak)
        : absolute_scale * sqrtf((float)count);
    if (value > 255.0f) value = 255.0f;
    return (uint8_t)lroundf(value);
}

static int waveform_level_y(int top, int bottom, double level)
{
    double t = (level - PROBE_WAVEFORM_MIN_SIGNAL) /
               (PROBE_WAVEFORM_MAX_SIGNAL - PROBE_WAVEFORM_MIN_SIGNAL);
    return bottom - (int)llround(t * (bottom - top));
}

static int build_waveform_panel(Renderer *r, Source *sources, int n,
                                int src, pl_gpu gpu, LayoutRect dst)
{
    if (src < 0 || src >= n || !sources[src].shown) return -1;
    const ProbeRegion *region = scope_region_for(r, src);

    int W = (int)lroundf(dst.x1 - dst.x0);
    int H = (int)lroundf(dst.y1 - dst.y0);
    if (W < 180 || H < 120) return -1;
    bool resized = slots[SLOT_SCOPE].W != W || slots[SLOT_SCOPE].H != H;
    ensure_slot(SLOT_SCOPE, gpu, W, H);
    if (!slots[SLOT_SCOPE].tex) return -1;

    bool rebuild = resized || scope_cache_view != HDRPLAY_SCOPE_WAVEFORM ||
                   scope_cache_src != src ||
                   scope_cache_frame != sources[src].frame_no ||
                   scope_cache_w != W || scope_cache_h != H ||
                   scope_region_changed(r, src);
    if (!rebuild) {
        position_slot(SLOT_SCOPE, (int)lroundf(dst.x0),
                      (int)lroundf(dst.y0));
        return 0;
    }

    uint8_t *buf = make_panel(W, H, 225);
    if (!buf) return -1;
    int detail_scale = W >= 1000 && H >= 500 ? 2 : 1;
    int title_scale = W >= 1000 && H >= 400 ? 3 : 2;
    int left = detail_scale > 1 ? 52 : 46;
    int right = W - 12;
    int top = 12 + FONT_H * title_scale;
    int bottom = H - 12 - FONT_H * detail_scale;
    int plot_w = right - left + 1;
    int plot_h = bottom - top + 1;
    size_t plane_size = (size_t)plot_w * (size_t)plot_h;
    uint32_t *bins = calloc(3 * plane_size, sizeof(*bins));
    if (!bins) { free(buf); return -1; }

    /* Border and 0/25/50/75/100% grid. The plot itself includes ten
     * percent guard bands at top and bottom for illegal excursions. */
    for (int x = left; x <= right; x++) {
        for (int edge = 0; edge < 2; edge++) {
            int y = edge ? bottom : top;
            uint8_t *p = &buf[(y * W + x) * 4];
            p[0] = p[1] = p[2] = 70; p[3] = 235;
        }
    }
    for (int y = top; y <= bottom; y++) {
        for (int edge = 0; edge < 2; edge++) {
            int x = edge ? right : left;
            uint8_t *p = &buf[(y * W + x) * 4];
            p[0] = p[1] = p[2] = 70; p[3] = 235;
        }
    }
    for (int level = 0; level <= 100; level += 25) {
        int y = waveform_level_y(top, bottom, level / 100.0);
        for (int x = left; x <= right; x++) {
            uint8_t *p = &buf[(y * W + x) * 4];
            uint8_t grid = level == 0 || level == 100 ? 80 : 42;
            p[0] = p[1] = p[2] = grid; p[3] = 235;
        }
        char label[8];
        snprintf(label, sizeof(label), "%d", level);
        draw_text_color(buf, W, H, 4, y - 4 * detail_scale,
                        detail_scale, label,
                        175, 175, 175);
    }

    int glyph_advance = (FONT_W + 1) * title_scale;
    draw_text_color(buf, W, H, 8, 7, title_scale, "R", 255, 80, 80);
    draw_text_color(buf, W, H, 8 + glyph_advance, 7, title_scale,
                    "G", 80, 255, 80);
    draw_text_color(buf, W, H, 8 + 2 * glyph_advance, 7, title_scale,
                    "B", 80, 120, 255);
    draw_text_color(buf, W, H, 8 + 3 * glyph_advance, 7, title_scale,
                    region ? " WAVEFORM ROI NORM" : " WAVEFORM",
                    235, 235, 235);
    draw_text_color(buf, W, H, W - 8 - 9 * (FONT_W + 1) * detail_scale,
                    7, detail_scale, "-10..110%", 150, 150, 150);
    char source_label[40];
    snprintf(source_label, sizeof(source_label), "SOURCE %.28s",
             sources[src].label);
    draw_text_color(buf, W, H, left, H - 6 - FONT_H * detail_scale,
                    detail_scale, source_label,
                    150, 150, 150);

    uint32_t peaks[3];
    int samples = 0;
    bool ok = probe_rgb_waveform(sources[src].shown, region,
                                 plot_w, plot_h, 4,
                                 bins, peaks, &samples);
    if (ok) {
        uint32_t common_peak = peaks[0];
        if (peaks[1] > common_peak) common_peak = peaks[1];
        if (peaks[2] > common_peak) common_peak = peaks[2];
        for (int c = 0; c < 3; c++) {
            for (int y = 0; y < plot_h; y++) {
                for (int x = 0; x < plot_w; x++) {
                    uint32_t count = bins[((size_t)c * plot_h + y) *
                                          plot_w + x];
                    if (!count) continue;
                    uint8_t intensity = scope_density(count, common_peak,
                                                      36.0f, region != NULL);
                    scope_pixel(buf, W, H, left + x, top + y,
                                c, intensity);
                    /* A faint neighbor keeps single-sample traces visible
                     * on high-DPI displays without smearing the density. */
                    uint8_t halo = (uint8_t)(intensity / 3);
                    scope_pixel(buf, W, H, left + x, top + y - 1, c, halo);
                    scope_pixel(buf, W, H, left + x, top + y + 1, c, halo);
                }
            }
        }
    } else {
        draw_text_color(buf, W, H, left + 12, top + 12, 1,
                        "WAVEFORM UNAVAILABLE FOR PIXEL FORMAT",
                        255, 120, 80);
    }
    free(bins);

    commit_slot(SLOT_SCOPE, gpu, buf,
                (int)lroundf(dst.x0), (int)lroundf(dst.y0));
    free(buf);
    scope_cache_src = src;
    scope_cache_frame = sources[src].frame_no;
    scope_cache_w = W;
    scope_cache_h = H;
    scope_cache_view = HDRPLAY_SCOPE_WAVEFORM;
    scope_region_cache(r, src);
    return 0;
}

typedef struct { double x, y; } ScopeXY;

static void gamut_to_pixel(double x, double y,
                           int left, int top, int plot_w, int plot_h,
                           int *px, int *py)
{
    *px = left + (int)lround(x / PROBE_GAMUT_X_MAX * (plot_w - 1));
    *py = top + plot_h - 1 -
          (int)lround(y / PROBE_GAMUT_Y_MAX * (plot_h - 1));
}

static void draw_scope_line(uint8_t *buf, int W, int H,
                            int x0, int y0, int x1, int y1,
                            uint8_t red, uint8_t green, uint8_t blue)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H) {
            uint8_t *p = &buf[(y0 * W + x0) * 4];
            if (red   > p[0]) p[0] = red;
            if (green > p[1]) p[1] = green;
            if (blue  > p[2]) p[2] = blue;
            if (p[3] < 235) p[3] = 235;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_gamut_triangle(uint8_t *buf, int W, int H,
                                int left, int top, int plot_w, int plot_h,
                                const ScopeXY tri[3],
                                uint8_t red, uint8_t green, uint8_t blue)
{
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        int x0, y0, x1, y1;
        gamut_to_pixel(tri[i].x, tri[i].y, left, top, plot_w, plot_h,
                       &x0, &y0);
        gamut_to_pixel(tri[j].x, tri[j].y, left, top, plot_w, plot_h,
                       &x1, &y1);
        draw_scope_line(buf, W, H, x0, y0, x1, y1, red, green, blue);
    }
}

static void xy_display_rgb(double x, double y, uint8_t out[3])
{
    if (!(y > 1e-9)) { out[0] = out[1] = out[2] = 180; return; }
    double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    double rgb[3] = {
         3.2406 * X - 1.5372 * Y - 0.4986 * Z,
        -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
         0.0557 * X - 0.2040 * Y + 1.0570 * Z,
    };
    double hi = fmax(rgb[0], fmax(rgb[1], rgb[2]));
    if (!(hi > 0.0)) hi = 1.0;
    for (int c = 0; c < 3; c++) {
        double v = rgb[c] / hi;
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        v = v <= 0.0031308 ? 12.92 * v
                           : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
        out[c] = (uint8_t)lround(v * 255.0);
    }
}

static int build_gamut_panel(Renderer *r, Source *sources, int n,
                             int src, pl_gpu gpu, LayoutRect dst)
{
    if (src < 0 || src >= n || !sources[src].shown) return -1;
    const ProbeRegion *region = scope_region_for(r, src);

    int W = (int)lroundf(dst.x1 - dst.x0);
    int H = (int)lroundf(dst.y1 - dst.y0);
    if (W < 240 || H < 180) return -1;
    bool resized = slots[SLOT_SCOPE].W != W || slots[SLOT_SCOPE].H != H;
    ensure_slot(SLOT_SCOPE, gpu, W, H);
    if (!slots[SLOT_SCOPE].tex) return -1;
    bool rebuild = resized || scope_cache_view != HDRPLAY_SCOPE_GAMUT ||
                   scope_cache_src != src ||
                   scope_cache_frame != sources[src].frame_no ||
                   scope_cache_w != W || scope_cache_h != H ||
                   scope_region_changed(r, src);
    if (!rebuild) {
        position_slot(SLOT_SCOPE, (int)lroundf(dst.x0),
                      (int)lroundf(dst.y0));
        return 0;
    }

    uint8_t *buf = make_panel(W, H, 225);
    if (!buf) return -1;
    int detail_scale = W >= 1000 && H >= 500 ? 2 : 1;
    int title_scale = W >= 1000 && H >= 400 ? 3 : 2;
    int top = 12 + FONT_H * title_scale;
    int bottom = H - 18;
    int left = 46;
    int available_w = W - left - 18;
    int available_h = bottom - top + 1;
    double scale = fmin(available_w / PROBE_GAMUT_X_MAX,
                        available_h / PROBE_GAMUT_Y_MAX);
    int plot_w = (int)floor(PROBE_GAMUT_X_MAX * scale);
    int plot_h = (int)floor(PROBE_GAMUT_Y_MAX * scale);
    int right = left + plot_w - 1;
    int plot_bottom = top + plot_h - 1;

    for (int tenth = 0; tenth <= 8; tenth++) {
        int x, unused;
        gamut_to_pixel(tenth / 10.0, 0.0, left, top, plot_w, plot_h,
                       &x, &unused);
        draw_scope_line(buf, W, H, x, top, x, plot_bottom, 34, 34, 34);
    }
    for (int tenth = 0; tenth <= 9; tenth++) {
        int unused, y;
        gamut_to_pixel(0.0, tenth / 10.0, left, top, plot_w, plot_h,
                       &unused, &y);
        draw_scope_line(buf, W, H, left, y, right, y, 34, 34, 34);
    }

    int bin_h = plot_h < 512 ? plot_h : 512;
    int bin_w = (int)lround(bin_h * PROBE_GAMUT_X_MAX /
                            PROBE_GAMUT_Y_MAX);
    if (bin_w > plot_w) bin_w = plot_w;
    size_t bin_count = (size_t)bin_w * (size_t)bin_h;
    uint32_t *bins = calloc(bin_count, sizeof(*bins));
    if (!bins) { free(buf); return -1; }
    uint32_t peak = 0;
    ProbeGamutStats stats;
    bool ok = probe_xy_gamut(sources[src].shown, region, bin_w, bin_h, 8,
                             bins, &peak, &stats);
    if (ok) {
        for (int by = 0; by < bin_h; by++) {
            for (int bx = 0; bx < bin_w; bx++) {
                uint32_t count = bins[(size_t)by * bin_w + bx];
                if (!count) continue;
                uint8_t intensity = scope_density(count, peak, 48.0f,
                                                  region != NULL);
                double x = ((double)bx + 0.5) / bin_w * PROBE_GAMUT_X_MAX;
                double y = (1.0 - ((double)by + 0.5) / bin_h) *
                           PROBE_GAMUT_Y_MAX;
                uint8_t color[3];
                xy_display_rgb(x, y, color);
                int x0 = left + bx * plot_w / bin_w;
                int x1 = left + (bx + 1) * plot_w / bin_w;
                int y0 = top + by * plot_h / bin_h;
                int y1 = top + (by + 1) * plot_h / bin_h;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                for (int py = y0; py < y1; py++)
                    for (int px = x0; px < x1; px++)
                        for (int c = 0; c < 3; c++)
                            scope_pixel(buf, W, H, px, py, c,
                                (uint8_t)(color[c] * intensity / 255));
            }
        }
    }
    free(bins);

    static const ScopeXY locus[] = {
        { .1741, .0050 }, { .1733, .0048 }, { .1689, .0069 },
        { .1440, .0297 }, { .0913, .1327 }, { .0454, .2950 },
        { .0082, .5384 }, { .0139, .7502 }, { .0743, .8338 },
        { .1547, .8059 }, { .2296, .7543 }, { .3016, .6923 },
        { .3731, .6245 }, { .4441, .5547 }, { .5125, .4866 },
        { .5752, .4242 }, { .6270, .3725 }, { .6658, .3340 },
        { .6915, .3083 }, { .7079, .2920 }, { .7190, .2809 },
        { .7347, .2653 },
    };
    for (size_t i = 1; i < sizeof(locus) / sizeof(locus[0]); i++) {
        int x0, y0, x1, y1;
        gamut_to_pixel(locus[i - 1].x, locus[i - 1].y,
                       left, top, plot_w, plot_h, &x0, &y0);
        gamut_to_pixel(locus[i].x, locus[i].y,
                       left, top, plot_w, plot_h, &x1, &y1);
        draw_scope_line(buf, W, H, x0, y0, x1, y1, 100, 100, 100);
    }
    int lx0, ly0, lx1, ly1;
    gamut_to_pixel(locus[sizeof(locus) / sizeof(locus[0]) - 1].x,
                   locus[sizeof(locus) / sizeof(locus[0]) - 1].y,
                   left, top, plot_w, plot_h, &lx0, &ly0);
    gamut_to_pixel(locus[0].x, locus[0].y,
                   left, top, plot_w, plot_h, &lx1, &ly1);
    draw_scope_line(buf, W, H, lx0, ly0, lx1, ly1, 100, 100, 100);

    static const ScopeXY rec709[3] = {
        { .640, .330 }, { .300, .600 }, { .150, .060 },
    };
    static const ScopeXY p3[3] = {
        { .680, .320 }, { .265, .690 }, { .150, .060 },
    };
    static const ScopeXY rec2020[3] = {
        { .708, .292 }, { .170, .797 }, { .131, .046 },
    };
    draw_gamut_triangle(buf, W, H, left, top, plot_w, plot_h,
                        rec2020, 60, 190, 220);
    draw_gamut_triangle(buf, W, H, left, top, plot_w, plot_h,
                        p3, 220, 190, 60);
    draw_gamut_triangle(buf, W, H, left, top, plot_w, plot_h,
                        rec709, 210, 210, 210);
    int wx, wy;
    gamut_to_pixel(.3127, .3290, left, top, plot_w, plot_h, &wx, &wy);
    draw_scope_line(buf, W, H, wx - 3, wy, wx + 3, wy, 255, 255, 255);
    draw_scope_line(buf, W, H, wx, wy - 3, wx, wy + 3, 255, 255, 255);

    draw_text_color(buf, W, H, 8, 7, title_scale,
                    region ? "CIE XY GAMUT ROI NORM" : "CIE XY GAMUT",
                    235, 235, 235);
    int info_x = right + 16;
    if (info_x + 120 * detail_scale < W) {
        char line[80];
        int line_step = FONT_H * detail_scale + 8;
        draw_text_color(buf, W, H, info_x, top, detail_scale,
                        "PIXELS OUTSIDE", 190, 190, 190);
        double outside709 = stats.samples
            ? 100.0 * stats.outside_709 / stats.samples : 0.0;
        double outsidep3 = stats.samples
            ? 100.0 * stats.outside_p3 / stats.samples : 0.0;
        snprintf(line, sizeof(line), "709  %.2f%%", outside709);
        draw_text_color(buf, W, H, info_x, top + line_step,
                        detail_scale, line,
                        220, 220, 220);
        snprintf(line, sizeof(line), "P3   %.2f%%", outsidep3);
        draw_text_color(buf, W, H, info_x, top + 2 * line_step,
                        detail_scale, line,
                        235, 205, 70);
        draw_text_color(buf, W, H, info_x, top + 4 * line_step,
                        detail_scale,
                        "TRIANGLES", 190, 190, 190);
        draw_text_color(buf, W, H, info_x, top + 5 * line_step,
                        detail_scale,
                        "709", 220, 220, 220);
        draw_text_color(buf, W, H, info_x, top + 6 * line_step,
                        detail_scale,
                        "P3", 235, 205, 70);
        draw_text_color(buf, W, H, info_x, top + 7 * line_step,
                        detail_scale,
                        "2020", 60, 190, 220);
        snprintf(line, sizeof(line), "TAG %s",
                 av_color_primaries_name(sources[src].shown->color_primaries)
                    ?: "UNSPECIFIED");
        draw_text_color(buf, W, H, info_x, top + 9 * line_step,
                        detail_scale, line,
                        170, 170, 170);
    }
    if (!ok)
        draw_text_color(buf, W, H, left + 12, top + 12, 1,
                        "GAMUT UNAVAILABLE FOR PIXEL FORMAT",
                        255, 120, 80);

    commit_slot(SLOT_SCOPE, gpu, buf,
                (int)lroundf(dst.x0), (int)lroundf(dst.y0));
    free(buf);
    scope_cache_src = src;
    scope_cache_frame = sources[src].frame_no;
    scope_cache_w = W;
    scope_cache_h = H;
    scope_cache_view = HDRPLAY_SCOPE_GAMUT;
    scope_region_cache(r, src);
    return 0;
}

static void vector_to_pixel(double cb, double cr,
                            int left, int top, int size,
                            int *px, int *py)
{
    double nx = (cb + PROBE_VECTOR_LIMIT) / (2.0 * PROBE_VECTOR_LIMIT);
    double ny = (cr + PROBE_VECTOR_LIMIT) / (2.0 * PROBE_VECTOR_LIMIT);
    *px = left + (int)lround(nx * (size - 1));
    *py = top + size - 1 - (int)lround(ny * (size - 1));
}

static void draw_vector_circle(uint8_t *buf, int W, int H,
                               int left, int top, int size, double radius,
                               uint8_t level)
{
    int old_x = 0, old_y = 0;
    for (int i = 0; i <= 96; i++) {
        double a = i * (2.0 * 3.14159265358979323846 / 96.0);
        int x, y;
        vector_to_pixel(radius * cos(a), radius * sin(a),
                        left, top, size, &x, &y);
        if (i)
            draw_scope_line(buf, W, H, old_x, old_y, x, y,
                            level, level, level);
        old_x = x; old_y = y;
    }
}

static int build_vector_panel(Renderer *r, Source *sources, int n,
                              int src, pl_gpu gpu, LayoutRect dst)
{
    if (src < 0 || src >= n || !sources[src].shown) return -1;
    AVFrame *frame = sources[src].shown;
    const ProbeRegion *region = scope_region_for(r, src);

    int W = (int)lroundf(dst.x1 - dst.x0);
    int H = (int)lroundf(dst.y1 - dst.y0);
    if (W < 240 || H < 180) return -1;
    bool resized = slots[SLOT_SCOPE].W != W || slots[SLOT_SCOPE].H != H;
    ensure_slot(SLOT_SCOPE, gpu, W, H);
    if (!slots[SLOT_SCOPE].tex) return -1;
    bool rebuild = resized || scope_cache_view != HDRPLAY_SCOPE_VECTOR ||
                   scope_cache_src != src ||
                   scope_cache_frame != sources[src].frame_no ||
                   scope_cache_w != W || scope_cache_h != H ||
                   fabsf(scope_cache_vector_gain - r->vector_gain) > 0.01f ||
                   scope_region_changed(r, src);
    if (!rebuild) {
        position_slot(SLOT_SCOPE, (int)lroundf(dst.x0),
                      (int)lroundf(dst.y0));
        return 0;
    }

    uint8_t *buf = make_panel(W, H, 225);
    if (!buf) return -1;
    int detail_scale = W >= 1000 && H >= 500 ? 2 : 1;
    int title_scale = W >= 1000 && H >= 400 ? 3 : 2;
    int top = 12 + FONT_H * title_scale;
    int left = 46;
    int info_reserve = detail_scale > 1 ? 330 : 180;
    int plot_size = H - top - 24;
    int max_w = W - left - info_reserve - 16;
    if (plot_size > max_w) plot_size = max_w;
    if (plot_size < 100) { free(buf); return -1; }
    int right = left + plot_size - 1;
    int bottom = top + plot_size - 1;
    int cx, cy;
    vector_to_pixel(0.0, 0.0, left, top, plot_size, &cx, &cy);

    draw_scope_line(buf, W, H, left, cy, right, cy, 55, 55, 55);
    draw_scope_line(buf, W, H, cx, top, cx, bottom, 55, 55, 55);
    draw_vector_circle(buf, W, H, left, top, plot_size, .25, 38);
    draw_vector_circle(buf, W, H, left, top, plot_size, .50, 70);

    int bin_size = plot_size < 512 ? plot_size : 512;
    size_t plane_size = (size_t)bin_size * (size_t)bin_size;
    size_t bin_count = PROBE_VECTOR_BANDS * plane_size;
    uint32_t *bins = calloc(bin_count, sizeof(*bins));
    if (!bins) { free(buf); return -1; }
    uint32_t peaks[PROBE_VECTOR_BANDS] = {0};
    ProbeVectorStats stats;
    bool ok = probe_cbcr_vectorscope(frame, region,
                                     bin_size, bin_size, 8,
                                     r->vector_gain,
                                     bins, peaks, &stats);
    if (ok) {
        static const uint8_t band_color[PROBE_VECTOR_BANDS][3] = {
            { 70, 120, 255 }, /* shadows   */
            { 70, 255, 110 }, /* midtones  */
            {255, 215,  85 }, /* highlights */
        };
        uint32_t common_peak = peaks[0];
        if (peaks[1] > common_peak) common_peak = peaks[1];
        if (peaks[2] > common_peak) common_peak = peaks[2];
        for (int band = 0; band < PROBE_VECTOR_BANDS; band++) {
            for (int by = 0; by < bin_size; by++) {
                for (int bx = 0; bx < bin_size; bx++) {
                    uint32_t count = bins[(size_t)band * plane_size +
                                          (size_t)by * bin_size + bx];
                    if (!count) continue;
                    uint8_t intensity = scope_density(count, common_peak,
                                                      48.0f, region != NULL);
                    int x0 = left + bx * plot_size / bin_size;
                    int x1 = left + (bx + 1) * plot_size / bin_size;
                    int y0 = top + by * plot_size / bin_size;
                    int y1 = top + (by + 1) * plot_size / bin_size;
                    if (x1 <= x0) x1 = x0 + 1;
                    if (y1 <= y0) y1 = y0 + 1;
                    for (int py = y0; py < y1; py++)
                        for (int px = x0; px < x1; px++)
                            for (int c = 0; c < 3; c++)
                                scope_pixel(buf, W, H, px, py, c,
                                    (uint8_t)(band_color[band][c] *
                                              intensity / 255));
                }
            }
        }
    }
    free(bins);

    static const char *target_names[6] = { "R", "MG", "B", "CY", "G", "Y" };
    static const uint8_t target_colors[6][3] = {
        {255, 70, 70}, {255, 70, 255}, {80, 100, 255},
        {70, 230, 230}, {70, 230, 70}, {240, 220, 70},
    };
    double targets[6][2];
    bool targets_ok = probe_vectorscope_targets(frame->colorspace, .75,
                                                 targets);
    if (targets_ok) {
        int box = detail_scale > 1 ? 6 : 4;
        for (int i = 0; i < 6; i++) {
            int tx, ty;
            vector_to_pixel(targets[i][0], targets[i][1],
                            left, top, plot_size, &tx, &ty);
            draw_scope_line(buf, W, H, tx - box, ty - box,
                            tx + box, ty - box,
                            target_colors[i][0], target_colors[i][1],
                            target_colors[i][2]);
            draw_scope_line(buf, W, H, tx + box, ty - box,
                            tx + box, ty + box,
                            target_colors[i][0], target_colors[i][1],
                            target_colors[i][2]);
            draw_scope_line(buf, W, H, tx + box, ty + box,
                            tx - box, ty + box,
                            target_colors[i][0], target_colors[i][1],
                            target_colors[i][2]);
            draw_scope_line(buf, W, H, tx - box, ty + box,
                            tx - box, ty - box,
                            target_colors[i][0], target_colors[i][1],
                            target_colors[i][2]);
            draw_text_color(buf, W, H, tx + box + 3,
                            ty - 4 * detail_scale, detail_scale,
                            target_names[i], target_colors[i][0],
                            target_colors[i][1], target_colors[i][2]);
        }
    }

    /* Conventional flesh-tone / I-line indicator. This is a practical
     * camera-grading heuristic at about 123 degrees, not a normative skin
     * chromaticity and not a detector of whether a pixel is skin. */
    const double skin_angle = 123.0 * 3.14159265358979323846 / 180.0;
    int skin_x, skin_y;
    vector_to_pixel(.50 * cos(skin_angle), .50 * sin(skin_angle),
                    left, top, plot_size, &skin_x, &skin_y);
    draw_scope_line(buf, W, H, cx, cy, skin_x, skin_y, 210, 150, 100);
    draw_text_color(buf, W, H, skin_x + 4,
                    skin_y - 4 * detail_scale, detail_scale,
                    "SKIN", 220, 160, 105);

    draw_text_color(buf, W, H, 8, 7, title_scale,
                    region ? "CB CR VECTORSCOPE ROI NORM" :
                             "CB CR VECTORSCOPE",
                    235, 235, 235);
    draw_text_color(buf, W, H, right - 10 * (FONT_W + 1) * detail_scale,
                    cy + 5, detail_scale, "+CB", 140, 140, 140);
    draw_text_color(buf, W, H, cx + 5, top + 3, detail_scale,
                    "+CR", 140, 140, 140);
    int info_x = right + 16;
    if (info_x + 120 * detail_scale < W) {
        char line[96];
        int line_step = FONT_H * detail_scale + 8;
        draw_text_color(buf, W, H, info_x, top, detail_scale,
                        "SIGNAL CHROMA", 190, 190, 190);
        double outside = stats.samples
            ? 100.0 * stats.outside_nominal / stats.samples : 0.0;
        snprintf(line, sizeof(line), "OUTSIDE %.2f%%", outside);
        draw_text_color(buf, W, H, info_x, top + line_step,
                        detail_scale, line, 180, 220, 180);
        snprintf(line, sizeof(line), "MATRIX %s",
                 av_color_space_name(frame->colorspace) ?: "UNSPECIFIED");
        draw_text_color(buf, W, H, info_x, top + 3 * line_step,
                        detail_scale, line, 170, 170, 170);
        snprintf(line, sizeof(line), "TRACE %.0fX", r->vector_gain);
        draw_text_color(buf, W, H, info_x, top + 4 * line_step,
                        detail_scale, line, 180, 220, 180);
        draw_text_color(buf, W, H, info_x, top + 5 * line_step,
                        detail_scale,
                        targets_ok ? "75% TARGETS" : "TARGETS N/A FOR CL",
                        190, 190, 190);
        draw_text_color(buf, W, H, info_x, top + 7 * line_step,
                        detail_scale, "Y BANDS", 190, 190, 190);
        snprintf(line, sizeof(line), "SHADOW <25 %.0f%%",
                 stats.samples ? 100.0 * stats.luma_band[0] / stats.samples
                               : 0.0);
        draw_text_color(buf, W, H, info_x, top + 8 * line_step,
                        detail_scale, line, 70, 120, 255);
        snprintf(line, sizeof(line), "MID 25-75 %.0f%%",
                 stats.samples ? 100.0 * stats.luma_band[1] / stats.samples
                               : 0.0);
        draw_text_color(buf, W, H, info_x, top + 9 * line_step,
                        detail_scale, line, 70, 255, 110);
        snprintf(line, sizeof(line), "HIGH >75 %.0f%%",
                 stats.samples ? 100.0 * stats.luma_band[2] / stats.samples
                               : 0.0);
        draw_text_color(buf, W, H, info_x, top + 10 * line_step,
                        detail_scale, line, 255, 215, 85);
    }
    if (!ok)
        draw_text_color(buf, W, H, left + 12, top + 12, 1,
                        "VECTOR UNAVAILABLE FOR PIXEL FORMAT",
                        255, 120, 80);

    commit_slot(SLOT_SCOPE, gpu, buf,
                (int)lroundf(dst.x0), (int)lroundf(dst.y0));
    free(buf);
    scope_cache_src = src;
    scope_cache_frame = sources[src].frame_no;
    scope_cache_w = W;
    scope_cache_h = H;
    scope_cache_view = HDRPLAY_SCOPE_VECTOR;
    scope_cache_vector_gain = r->vector_gain;
    scope_region_cache(r, src);
    return 0;
}

static int histogram_level_x(int left, int right, double level)
{
    double t = (level - PROBE_WAVEFORM_MIN_SIGNAL) /
               (PROBE_WAVEFORM_MAX_SIGNAL - PROBE_WAVEFORM_MIN_SIGNAL);
    return left + (int)llround(t * (right - left));
}

static int build_histogram_panel(Renderer *r, Source *sources, int n,
                                 int src, pl_gpu gpu, LayoutRect dst)
{
    if (src < 0 || src >= n || !sources[src].shown) return -1;
    const ProbeRegion *region = scope_region_for(r, src);

    int W = (int)lroundf(dst.x1 - dst.x0);
    int H = (int)lroundf(dst.y1 - dst.y0);
    if (W < 180 || H < 120) return -1;
    bool resized = slots[SLOT_SCOPE].W != W || slots[SLOT_SCOPE].H != H;
    ensure_slot(SLOT_SCOPE, gpu, W, H);
    if (!slots[SLOT_SCOPE].tex) return -1;
    bool rebuild = resized || scope_cache_view != HDRPLAY_SCOPE_HISTOGRAM ||
                   scope_cache_src != src ||
                   scope_cache_frame != sources[src].frame_no ||
                   scope_cache_w != W || scope_cache_h != H ||
                   scope_region_changed(r, src);
    if (!rebuild) {
        position_slot(SLOT_SCOPE, (int)lroundf(dst.x0),
                      (int)lroundf(dst.y0));
        return 0;
    }

    uint8_t *buf = make_panel(W, H, 225);
    if (!buf) return -1;
    int detail_scale = W >= 1000 && H >= 500 ? 2 : 1;
    int title_scale = W >= 1000 && H >= 400 ? 3 : 2;
    int left = detail_scale > 1 ? 58 : 46;
    int right = W - 14;
    int top = 12 + FONT_H * title_scale;
    int bottom = H - 16 - FONT_H * detail_scale;
    int plot_w = right - left + 1;
    int plot_h = bottom - top + 1;
    if (plot_w < 64 || plot_h < 48) { free(buf); return -1; }

    /* Log-frequency grid. The horizontal divisions describe relative
     * display height, not linear sample percentages. */
    for (int q = 0; q <= 4; q++) {
        int y = bottom - q * (bottom - top) / 4;
        draw_scope_line(buf, W, H, left, y, right, y,
                        q == 0 || q == 4 ? 70 : 36,
                        q == 0 || q == 4 ? 70 : 36,
                        q == 0 || q == 4 ? 70 : 36);
    }
    static const int levels[] = { 0, 25, 50, 75, 100 };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        int x = histogram_level_x(left, right, levels[i] / 100.0);
        uint8_t grid = levels[i] == 0 || levels[i] == 100 ? 76 : 38;
        draw_scope_line(buf, W, H, x, top, x, bottom, grid, grid, grid);
        char label[8];
        snprintf(label, sizeof(label), "%d", levels[i]);
        int label_w = (int)strlen(label) * (FONT_W + 1) * detail_scale;
        draw_text_color(buf, W, H, x - label_w / 2,
                        bottom + 5, detail_scale, label,
                        165, 165, 165);
    }
    draw_scope_line(buf, W, H, left, top, left, bottom, 70, 70, 70);
    draw_scope_line(buf, W, H, right, top, right, bottom, 70, 70, 70);

    /* A histogram is for seeing distribution trends, not the source code
     * lattice. A fixed 256 bins gives about 0.47 percentage-point signal
     * resolution across the full -10%..110% guard range while remaining
     * legible on high-resolution displays. */
    const int bin_count = 256;
    uint32_t *bins = calloc(3 * (size_t)bin_count, sizeof(*bins));
    if (!bins) { free(buf); return -1; }
    uint32_t peaks[3] = {0};
    ProbeRgbHistogramStats stats;
    bool ok = probe_rgb_histogram(sources[src].shown, region, bin_count, 8,
                                  bins, peaks, &stats);
    uint32_t common_peak = peaks[0];
    if (peaks[1] > common_peak) common_peak = peaks[1];
    if (peaks[2] > common_peak) common_peak = peaks[2];
    if (ok && common_peak) {
        double log_peak = log1p((double)common_peak);
        for (int c = 0; c < 3; c++) {
            for (int bx = 0; bx < bin_count; bx++) {
                uint32_t count = bins[(size_t)c * bin_count + bx];
                if (!count) continue;
                double density = log1p((double)count) / log_peak;
                int y0 = bottom - (int)llround(density * (plot_h - 1));
                int x0 = left + bx * plot_w / bin_count;
                int x1 = left + (bx + 1) * plot_w / bin_count;
                if (x1 <= x0) x1 = x0 + 1;
                for (int x = x0; x < x1; x++) {
                    for (int y = y0; y <= bottom; y++)
                        scope_pixel(buf, W, H, x, y, c, 48);
                    scope_pixel(buf, W, H, x, y0, c, 255);
                    scope_pixel(buf, W, H, x, y0 + 1, c, 150);
                }
            }
        }
    }
    free(bins);

    int glyph_advance = (FONT_W + 1) * title_scale;
    draw_text_color(buf, W, H, 8, 7, title_scale, "R", 255, 80, 80);
    draw_text_color(buf, W, H, 8 + glyph_advance, 7, title_scale,
                    "G", 80, 255, 80);
    draw_text_color(buf, W, H, 8 + 2 * glyph_advance, 7, title_scale,
                    "B", 80, 120, 255);
    draw_text_color(buf, W, H, 8 + 3 * glyph_advance, 7, title_scale,
                    " HISTOGRAM", 235, 235, 235);
    draw_text_color(buf, W, H,
                    W - 11 * (FONT_W + 1) * detail_scale,
                    7, detail_scale, "LOG COUNT", 155, 155, 155);
    if (ok) {
        char line[128];
        double denom = stats.samples ? (double)stats.samples : 1.0;
        snprintf(line, sizeof(line), "OUTSIDE R %.2f%% G %.2f%% B %.2f%%",
                 100.0 * stats.outside_nominal[0] / denom,
                 100.0 * stats.outside_nominal[1] / denom,
                 100.0 * stats.outside_nominal[2] / denom);
        draw_text_color(buf, W, H, left, top + 5, detail_scale, line,
                        180, 180, 180);
    } else {
        draw_text_color(buf, W, H, left + 12, top + 12, 1,
                        "HISTOGRAM UNAVAILABLE FOR PIXEL FORMAT",
                        255, 120, 80);
    }

    commit_slot(SLOT_SCOPE, gpu, buf,
                (int)lroundf(dst.x0), (int)lroundf(dst.y0));
    free(buf);
    scope_cache_src = src;
    scope_cache_frame = sources[src].frame_no;
    scope_cache_w = W;
    scope_cache_h = H;
    scope_cache_view = HDRPLAY_SCOPE_HISTOGRAM;
    scope_region_cache(r, src);
    return 0;
}

static int build_scope_roi(Renderer *r, pl_gpu gpu)
{
    int src = renderer_focus_source(r);
    if (src < 0 || src >= 2 || !r->scope_roi_active[src] ||
        !r->focus_map_valid || r->focus_map_src != src)
        return -1;

    ProbeRegion roi = r->scope_roi[src];
    LayoutRect source = {
        (float)roi.x0, (float)roi.y0, (float)roi.x1, (float)roi.y1,
    };
    LayoutRect dst;
    if (!layout_source_to_window(source, r->focus_map_target,
                                 r->focus_map_image,
                                 r->focus_map_frame_w,
                                 r->focus_map_frame_h,
                                 r->focus_map_rotation, &dst))
        return -1;

    int x0 = (int)floorf(dst.x0), y0 = (int)floorf(dst.y0);
    int x1 = (int)ceilf(dst.x1),  y1 = (int)ceilf(dst.y1);
    if (x1 <= x0 || y1 <= y0) return -1;
    int thick = r->focus_map_win_w >= 2000 ? 4 : 2;
    if (x1 - x0 < 2 * thick) thick = 1;
    if (y1 - y0 < 2 * thick) thick = 1;

    ensure_slot(SLOT_SCOPE_ROI, gpu, 1, 1);
    if (!slots[SLOT_SCOPE_ROI].tex) return -1;
    uint8_t pixel[4] = { 255, 210, 30, 255 };
    pl_tex_upload(gpu, pl_tex_transfer_params(
        .tex = slots[SLOT_SCOPE_ROI].tex,
        .ptr = pixel,
    ));
    roi_parts[0] = (struct pl_overlay_part){
        .src = {0, 0, 1, 1}, .dst = {x0, y0, x1, y0 + thick} };
    roi_parts[1] = (struct pl_overlay_part){
        .src = {0, 0, 1, 1}, .dst = {x0, y1 - thick, x1, y1} };
    roi_parts[2] = (struct pl_overlay_part){
        .src = {0, 0, 1, 1}, .dst = {x0, y0, x0 + thick, y1} };
    roi_parts[3] = (struct pl_overlay_part){
        .src = {0, 0, 1, 1}, .dst = {x1 - thick, y0, x1, y1} };
    overlay_arr[SLOT_SCOPE_ROI] = (struct pl_overlay){
        .tex = slots[SLOT_SCOPE_ROI].tex,
        .mode = PL_OVERLAY_NORMAL,
        .parts = roi_parts,
        .num_parts = 4,
        .repr = pl_color_repr_rgb,
        .color = pl_color_space_srgb,
    };
    return 0;
}

/* Build the multi-line status panel at top-left. */
static int build_status_panel(Renderer *r, pl_gpu gpu, int win_w, int win_h)
{
    (void)win_w; (void)win_h;
    /* H must fit the worst case, which is SPLIT mode with the probe on:
     * MODE, DISPLAY HDR, src csp, out csp, FRAME, SRC PEAK, DR, SDR CAP,
     * ABOVE 500N, SDR BOOST, PROBE, RGB = 12 lines, plus the excessive-
     * headroom warning = 13. Line n starts at 6 + 24(n-1) and is 16px
     * tall, so 13 lines needs 6 + 24*12 + 16 = 310. At the old H = 220
     * anything past line 9 was silently dropped by draw_text_color's
     * bounds check — pressing M in split mode looked like a no-op
     * because both PROBE lines fell off the bottom. */
    /* Must match STATUS_W / STATUS_H in layout.c, which needs the rect
     * to decide which pass owns the panel. draw_text clips silently, so
     * a panel too short by one line loses that line with no warning. */
    const int W = 460, H = 364;
    ensure_slot(SLOT_STATUS, gpu, W, H);
    if (!slots[SLOT_STATUS].tex) return -1;
    uint8_t *buf = make_panel(W, H, 170);
    if (!buf) return -1;

    char line[160];
    int y = 6;

    const char *mode_str =
        r->diff_view ? (r->mode == HDRPLAY_MODE_SDR
                        ? "DIFF SDR 4X" : "DIFF HDR 4X") :
        r->mode == HDRPLAY_MODE_HDR ? "HDR" :
        r->mode == HDRPLAY_MODE_SDR ? "SDR" :
        r->mode == HDRPLAY_MODE_SPLIT
            ? (r->split_orient == HDRPLAY_SPLIT_TB      ? "SPLIT TB" :
               r->split_orient == HDRPLAY_SPLIT_DIAG    ? "SPLIT DIAG" :
               r->split_orient == HDRPLAY_SPLIT_WIPE_LR ? "WIPE LR" :
               r->split_orient == HDRPLAY_SPLIT_WIPE_TB ? "WIPE TB" :
                                                          "SPLIT LR")
            : "?";
    const char *plane_str =
        r->plane_view == HDRPLAY_PLANE_Y  ? " PLANE Y" :
        r->plane_view == HDRPLAY_PLANE_CB ? " PLANE CB" :
        r->plane_view == HDRPLAY_PLANE_CR ? " PLANE CR" :
        r->plane_view == HDRPLAY_PLANE_LEGAL ? " LEGAL RANGE" :
        r->plane_view == HDRPLAY_PLANE_CLIP ? " CLIP LITERAL" :
        r->plane_view == HDRPLAY_PLANE_PLATEAU ? " CLIP PLATEAU" : "";
    snprintf(line, sizeof(line), "MODE %s%s", mode_str, plane_str);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += FONT_H * hud_scale + 8;

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
    y += FONT_H * hud_scale + 8;

    int ambient_source = renderer_focus_source(r);
    if (r->frame_stats_valid &&
        r->frame_stats.reference == LUM_HLG_OOTF &&
        r->ambient_reference_effective[ambient_source] > 0.0f)
    {
        if (r->ambient_lux_current > 0.0f) {
            snprintf(line, sizeof(line),
                     "AMBIENT %.0fLUX REF %.0fLUX EXP %.2f NONSTD",
                     r->ambient_lux_current,
                     r->ambient_reference_effective[ambient_source],
                     r->ambient_exponent[ambient_source]);
        } else {
            snprintf(line, sizeof(line),
                     "AMBIENT UNAVAILABLE REF %.0fLUX",
                     r->ambient_reference_effective[ambient_source]);
        }
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 200, 80);
        y += FONT_H * hud_scale + 8;
    }

    if (excessive_headroom) {
        draw_text_color(buf, W, H, 6, y, hud_scale,
                        "DARK CONTENT MAY BE INVISIBLE", 255, 80, 80);
        y += FONT_H * hud_scale + 8;
    }

    /* Source colorspace, compact form. */
    if (r->last_source_csp[0])
        draw_text(buf, W, H, 6, y, hud_scale, r->last_source_csp);
    y += FONT_H * hud_scale + 8;

    /* Output target, compact form. */
    if (r->last_output_csp[0])
        draw_text(buf, W, H, 6, y, hud_scale, r->last_output_csp);
    y += FONT_H * hud_scale + 8;

    snprintf(line, sizeof(line), "FRAME %d%s%s",
             r->current_frame_no >= 0 ? r->current_frame_no : 0,
             r->paused       ? " PAUSED" : "",
             r->loop_enabled ? " LOOP"   : "");
    draw_text(buf, W, H, 6, y, hud_scale, line); y += FONT_H * hud_scale + 8;

    /* Displayed scale. Below 1:1 a resampler sits between you and the
     * pixels, so any artifact judgement is really a judgement about the
     * downscale; 1:1 is called out explicitly because that is the state
     * worth getting to and it is not distinguishable by eye. */
    if (r->last_scale > 0.0f) {
        if (fabsf(r->last_scale - 1.0f) < 0.001f)
            snprintf(line, sizeof(line), "SCALE 1:1 EXACT");
        else if (r->zoom > 0.0f)
            snprintf(line, sizeof(line), "SCALE %.3gX ZOOM", r->last_scale);
        else
            snprintf(line, sizeof(line), "SCALE %.3gX FIT", r->last_scale);
        draw_text(buf, W, H, 6, y, hud_scale, line); y += FONT_H * hud_scale + 8;
    }

    /* Per-frame source brightness stats — answers "does this frame
     * actually have HDR content to show?". Peak/avg/DR are always
     * shown; the "ABOVE SDR" line is green when > 0% (HDR-worthy
     * highlights present in this frame) and dim white when 0%
     * (this frame has nothing exceeding the SDR ceiling, so the
     * HDR-vs-SDR comparison will look identical on it). */
    if (r->frame_stats_valid) {
        snprintf(line, sizeof(line), "SRC PEAK %.0fN AVG %.0fN",
                 r->frame_stats.peak_nits, r->frame_stats.avg_nits);
        draw_text(buf, W, H, 6, y, hud_scale, line);
        y += FONT_H * hud_scale + 8;

        snprintf(line, sizeof(line), "DR %.1f STOPS",
                 r->frame_stats.dr_stops);
        draw_text(buf, W, H, 6, y, hud_scale, line);
        y += FONT_H * hud_scale + 8;

        /* SDR-pane DR cap — only meaningful when there's an SDR pane
         * on screen. Suppress in pure HDR mode (would just be a
         * counterfactual constant). Amber when source DR > cap (SDR
         * pane crushes some source shadow detail; HDR-advantage axis
         * active on this frame), dim white when source DR fits within
         * the cap (SDR can carry the full source DR; no shadow-side
         * advantage). */
        if (r->mode != HDRPLAY_MODE_HDR) {
            float cap = r->sdr_dr_stops_cap > 0.0f ? r->sdr_dr_stops_cap : 12.0f;
            snprintf(line, sizeof(line), "SDR CAP %.1f STOPS", cap);
            if (r->frame_stats.dr_stops > cap + 0.05)
                draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 200, 80);
            else
                draw_text_color(buf, W, H, 6, y, hud_scale, line, 140, 140, 140);
            y += FONT_H * hud_scale + 8;
        }

        snprintf(line, sizeof(line), "ABOVE 500N %.1fPCT",
                 r->frame_stats.pct_above_500);
        if (r->frame_stats.pct_above_500 > 0.01f)
            draw_text_color(buf, W, H, 6, y, hud_scale, line, 120, 255, 120);
        else
            draw_text_color(buf, W, H, 6, y, hud_scale, line, 140, 140, 140);
        y += FONT_H * hud_scale + 8;

        /* SDR BOOST — symmetric counterpart to ABOVE 500N. Fires when
         * source peak < SDR target peak: libplacebo's inverse_tone_mapping
         * EXPANDS the source up to fill the SDR target, so the SDR pane
         * renders this frame brighter than HDR does (which keeps source
         * at its true authored brightness). This is where HDR's most
         * honest perceptual win lives: mid-tone / low-DR scenes the SDR
         * pane has to inflate to look "normal", while HDR shows them at
         * their true nits. Only shown when an SDR pane is on screen. */
        if (r->mode != HDRPLAY_MODE_HDR &&
            r->sdr_peak_effective > 0.0f &&
            r->frame_stats.peak_nits > 0.0)
        {
            double boost_stops = log2(r->sdr_peak_effective /
                                      r->frame_stats.peak_nits);
            if (boost_stops < 0.0) boost_stops = 0.0;
            snprintf(line, sizeof(line), "SDR BOOST +%.1f STOPS",
                     boost_stops);
            if (boost_stops > 0.3)
                draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 200, 80);
            else
                draw_text_color(buf, W, H, 6, y, hud_scale, line, 140, 140, 140);
            y += FONT_H * hud_scale + 8;
        }
    }

    /* Luminance probe — reports nominal nits of the source pixel under
     * the cursor. Greenish so it stands out from the rest of the panel. */
    if (r->probe_active) {
        snprintf(line, sizeof(line), "PROBE (%d,%d) %.1f NITS",
                 r->probe_x, r->probe_y, r->probe_nits);
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 120, 255, 120);
        y += FONT_H * hud_scale + 8;
        snprintf(line, sizeof(line), "R %.1f G %.1f B %.1f",
                 r->probe_r_nits, r->probe_g_nits, r->probe_b_nits);
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 120, 255, 120);
        y += FONT_H * hud_scale + 8;
    }

    commit_slot(SLOT_STATUS, gpu, buf, 16, 16);
    free(buf);
    return 0;
}
/* ------------------------------------------------------------------ */
/* Accumulated-statistics panel.                                       */
/*                                                                     */
/* With one source this is a detail view of that file. With two it     */
/* becomes a side-by-side table, because in comparison mode the        */
/* interesting thing is the DIFFERENCE, not either column.             */
/*                                                                     */
/* Two presentation rules keep the numbers honest either way:          */
/*                                                                     */
/*  - Measured MaxCLL/MaxFALL are one-sided LOWER bounds (strided      */
/*    sampling, luma rather than maxRGB on this path, and Jensen on a  */
/*    convex EOTF all push the estimate down). They are prefixed MIN   */
/*    and only coloured red when they EXCEED the declared value, which */
/*    is the one direction that constitutes evidence.                  */
/*  - Absolute figures are suppressed for SDR sources, where a         */
/*    measured peak cannot exceed 100 nits by construction and so      */
/*    carries no information.                                          */
/* ------------------------------------------------------------------ */
static int build_session_panel_single(Renderer *r, pl_gpu gpu,
                                      LayoutRect dst, SessionStats *ss)
{
    SessionDerived d;
    session_stats_derive(ss, &d);

    const int W = (int)(dst.x1 - dst.x0), H = (int)(dst.y1 - dst.y0);
    ensure_slot(SLOT_SESSION, gpu, W, H);
    if (!slots[SLOT_SESSION].tex) return -1;
    uint8_t *buf = make_panel(W, H, 170);
    if (!buf) return -1;

    char line[160];
    int  y = 6;
    const int pitch = FONT_H * hud_scale + 8;
    bool absolute = lum_reference_is_absolute(d.reference);

    if (d.coverage >= 0.0)
        snprintf(line, sizeof(line), "SESSION %lluF COVER %.0fPCT",
                 (unsigned long long)d.frames, d.coverage * 100.0);
    else
        snprintf(line, sizeof(line), "SESSION %lluF COVER UNKNOWN",
                 (unsigned long long)d.frames);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += pitch;

    if (d.frames == 0) {
        draw_text_color(buf, W, H, 6, y, hud_scale,
                        "NO FRAMES MEASURED", 255, 200, 80);
        commit_slot(SLOT_SESSION, gpu, buf, (int)dst.x0, (int)dst.y0);
        free(buf);
        return 0;
    }

    if (absolute) {
        struct { const char *tag; double meas; int decl; bool has; } rows[] = {
            { "MaxCLL ", d.maxcll_nits,  r->declared_cll_max, r->has_declared_cll },
            { "MaxFALL", d.maxfall_nits, r->declared_cll_avg, r->has_declared_cll },
        };
        for (int i = 0; i < 2; i++) {
            if (rows[i].has)
                snprintf(line, sizeof(line), "%s MIN %.0fN DECL %dN",
                         rows[i].tag, rows[i].meas, rows[i].decl);
            else
                snprintf(line, sizeof(line), "%s MIN %.0fN DECL NONE",
                         rows[i].tag, rows[i].meas);
            if (rows[i].has && rows[i].meas > rows[i].decl)
                draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 80, 80);
            else
                draw_text(buf, W, H, 6, y, hud_scale, line);
            y += pitch;
        }
    } else {
        draw_text_color(buf, W, H, 6, y, hud_scale,
                        "SDR SOURCE - NO ABSOLUTE NITS", 140, 140, 140);
        y += pitch;
    }

    snprintf(line, sizeof(line), "P50 %.0fN  P1 %.0fN  P99 %.0fN",
             d.p50, d.p1, d.p99);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += pitch;

    /* Against the ceiling, not bare: "9.0 OF 9.7" says the file uses
     * its format, where a lone "9.0 STOPS" says nothing. A measurement
     * above the ceiling is not content — p1 is in the code lattice. */
    if (isfinite(d.dr_ceiling_stops) && d.dr_ceiling_stops > 0.0) {
        snprintf(line, sizeof(line), "DR %.1f OF %.1f STOPS (P99.9/P1)",
                 d.dr_stops, d.dr_ceiling_stops);
        if (d.dr_stops > d.dr_ceiling_stops + 0.25)
            draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 80, 80);
        else
            draw_text(buf, W, H, 6, y, hud_scale, line);
    } else {
        snprintf(line, sizeof(line), "DR %.1f STOPS (P99.9/P1)", d.dr_stops);
        draw_text(buf, W, H, 6, y, hud_scale, line);
    }
    y += pitch;

    snprintf(line, sizeof(line), "SPREAD %.1f SPAT / %.1f TEMP",
             d.spatial_stops, d.temporal_stops);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += pitch;

    snprintf(line, sizeof(line), "BLACK %.0fPCT  UNDER %.1fPCT",
             d.black_pct, d.underflow_pct);
    draw_text_color(buf, W, H, 6, y, hud_scale, line, 140, 140, 140);
    y += pitch;

    if (d.reference == LUM_HLG_OOTF) {
        snprintf(line, sizeof(line), "HLG ASSUMES LW %.0fN", d.hlg_lw);
        draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 200, 80);
        y += pitch;
    }

    commit_slot(SLOT_SESSION, gpu, buf, (int)dst.x0, (int)dst.y0);
    free(buf);
    return 0;
}

/* Two-column comparison. Rows where the two files differ meaningfully
 * are highlighted, because scanning two columns of numbers for a small
 * delta is exactly the thing a person is bad at. */
static int build_session_panel_pair(Renderer *r, pl_gpu gpu, LayoutRect dst,
                                    Source *sa, Source *sb)
{
    SessionDerived a, b;
    session_stats_derive(&sa->session, &a);
    session_stats_derive(&sb->session, &b);

    const int W = (int)(dst.x1 - dst.x0), H = (int)(dst.y1 - dst.y0);
    ensure_slot(SLOT_SESSION, gpu, W, H);
    if (!slots[SLOT_SESSION].tex) return -1;
    uint8_t *buf = make_panel(W, H, 170);
    if (!buf) return -1;

    char line[160];
    int  y = 6;
    const int pitch = FONT_H * hud_scale + 8;

    double cov = a.coverage >= 0.0 ? a.coverage : b.coverage;
    if (cov >= 0.0)
        snprintf(line, sizeof(line), "SESSION %lluF COVER %.0fPCT",
                 (unsigned long long)a.frames, cov * 100.0);
    else
        snprintf(line, sizeof(line), "SESSION %lluF", (unsigned long long)a.frames);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += pitch;

    /* Column header uses the first 6 characters of each basename —
     * enough to tell two encodes apart without wrapping the panel. */
    snprintf(line, sizeof(line), "         %-8.8s %-8.8s", sa->label, sb->label);
    draw_text_color(buf, W, H, 6, y, hud_scale, line, 180, 180, 180);
    y += pitch;

    if (a.frames == 0 || b.frames == 0) {
        draw_text_color(buf, W, H, 6, y, hud_scale,
                        "WAITING FOR FRAMES", 255, 200, 80);
        commit_slot(SLOT_SESSION, gpu, buf, (int)dst.x0, (int)dst.y0);
        free(buf);
        return 0;
    }

    bool absolute = lum_reference_is_absolute(a.reference) &&
                    lum_reference_is_absolute(b.reference);

    struct { const char *tag; double va, vb; const char *unit; double tol; bool show; } rows[] = {
        { "MaxCLL", a.maxcll_nits,  b.maxcll_nits,  "N", 0.02, absolute },
        { "MaxFALL",a.maxfall_nits, b.maxfall_nits, "N", 0.02, absolute },
        { "P50",    a.p50,          b.p50,          "N", 0.02, true },
        { "P99",    a.p99,          b.p99,          "N", 0.02, true },
        { "DR",     a.dr_stops,     b.dr_stops,     "",  0.02, true },
        { "SPREAD", a.spatial_stops,b.spatial_stops,"",  0.02, true },
    };

    for (size_t i = 0; i < sizeof(rows)/sizeof(*rows); i++) {
        if (!rows[i].show) continue;
        if (rows[i].unit[0])
            snprintf(line, sizeof(line), "%-8s %7.0f%s %7.0f%s",
                     rows[i].tag, rows[i].va, rows[i].unit,
                     rows[i].vb, rows[i].unit);
        else
            snprintf(line, sizeof(line), "%-8s %8.1f %8.1f",
                     rows[i].tag, rows[i].va, rows[i].vb);

        /* Relative difference, so the highlight means the same thing
         * whether the row is in nits or stops. */
        double denom = fabs(rows[i].va) > 1e-9 ? fabs(rows[i].va) : 1.0;
        double rel = fabs(rows[i].va - rows[i].vb) / denom;
        if (rel > rows[i].tol)
            draw_text_color(buf, W, H, 6, y, hud_scale, line, 255, 200, 80);
        else
            draw_text(buf, W, H, 6, y, hud_scale, line);
        y += pitch;
    }

    if (!absolute)
        draw_text_color(buf, W, H, 6, y, hud_scale,
                        "SDR - NO ABSOLUTE NITS", 140, 140, 140);

    commit_slot(SLOT_SESSION, gpu, buf, (int)dst.x0, (int)dst.y0);
    free(buf);
    return 0;
}

/* Build a badge with a big label and a subtitle, so each pane is
 * identifiable from across the room. */
static int build_label_badge(int slot, pl_gpu gpu, const char *big,
                             const char *sub, LayoutRect dst)
{
    const int W = (int)(dst.x1 - dst.x0), H = (int)(dst.y1 - dst.y0);
    ensure_slot(slot, gpu, W, H);
    if (!slots[slot].tex) return -1;
    uint8_t *buf = make_panel(W, H, 200);
    if (!buf) return -1;

    /* Big label at 4x so it reads at a glance; subtitle beneath. */
    draw_text(buf, W, H, 12, 8, 4, big);
    if (sub) draw_text(buf, W, H, 12, 8 + FONT_H * 4 + 4, hud_scale, sub);

    commit_slot(slot, gpu, buf, (int)dst.x0, (int)dst.y0);
    free(buf);
    return 0;
}

/* Uppercase copy for the bitmap font, which has no lowercase glyphs. */
static void upper6(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point.                                                  */
/*                                                                      */
/* There is one pl_render_image per PANE, not per mode: single-file      */
/* renders one pass, a two-file LR/TB comparison renders two. Which     */
/* pass each overlay belongs to is decided in layout.c, not here — an    */
/* overlay attached to two passes would composite twice and blend its    */
/* alpha-170 background to ~0.89 instead of 0.667.                       */
/*                                                                      */
/* Overlays must be attached to the target BEFORE pl_render_image;       */
/* libplacebo composites them during the pass, so attaching afterwards   */
/* silently does nothing. That was the bug that hid the original HUD.    */
/* ------------------------------------------------------------------ */
void hud_prepare(Renderer *r, Source *sources, int n,
                 const LayoutPlan *plan, int win_w, int win_h,
                 HudOverlays *out)
{
    pl_gpu gpu = r->vulkan->gpu;
    memset(out, 0, sizeof(*out));
    out->win_w = win_w;
    out->win_h = win_h;

    bool pair = (n > 1 && r->solo < 0);
    int  ia   = r->swapped ? 1 : 0;
    int  ib   = r->swapped ? 0 : 1;

    /* Walk the plan so a panel is built exactly when the layout says it
     * exists, and positioned exactly where the layout put it. */
    for (int pi = 0; pi < plan->n_pass; pi++) {
        const LayoutPass *lp = &plan->pass[pi];
        for (int oi = 0; oi < lp->n_ov; oi++) {
            const LayoutOverlay *ov = &lp->ov[oi];
            switch (ov->kind) {
            case LAYOUT_OV_STATUS:
                if (build_status_panel(r, gpu, win_w, win_h) == 0) {
                    out->status     = overlay_arr[SLOT_STATUS];
                    out->has_status = true;
                }
                break;

            case LAYOUT_OV_SESSION: {
                int rc = pair
                    ? build_session_panel_pair(r, gpu, ov->dst,
                                               &sources[ia], &sources[ib])
                    : build_session_panel_single(r, gpu, ov->dst,
                                                 &sources[renderer_focus_source(r)].session);
                if (rc == 0) {
                    out->session     = overlay_arr[SLOT_SESSION];
                    out->has_session = true;
                }
                break;
            }

            case LAYOUT_OV_SCOPE: {
                int rc;
                if (r->scope_view == HDRPLAY_SCOPE_GAMUT)
                    rc = build_gamut_panel(r, sources, n, ov->src,
                                           gpu, ov->dst);
                else if (r->scope_view == HDRPLAY_SCOPE_VECTOR)
                    rc = build_vector_panel(r, sources, n, ov->src,
                                            gpu, ov->dst);
                else if (r->scope_view == HDRPLAY_SCOPE_HISTOGRAM)
                    rc = build_histogram_panel(r, sources, n, ov->src,
                                               gpu, ov->dst);
                else
                    rc = build_waveform_panel(r, sources, n, ov->src,
                                              gpu, ov->dst);
                if (rc == 0) {
                    out->scope = overlay_arr[SLOT_SCOPE];
                    out->has_scope = true;
                }
                break;
            }

            case LAYOUT_OV_SCOPE_ROI:
                if (build_scope_roi(r, gpu) == 0) {
                    out->scope_roi = overlay_arr[SLOT_SCOPE_ROI];
                    out->has_scope_roi = true;
                }
                break;

            case LAYOUT_OV_PLANE: {
                const char *big =
                    r->diff_view ? "DIFF" :
                    r->plane_view == HDRPLAY_PLANE_Y  ? "Y" :
                    r->plane_view == HDRPLAY_PLANE_CB ? "CB" :
                    r->plane_view == HDRPLAY_PLANE_CR ? "CR" :
                    r->plane_view == HDRPLAY_PLANE_LEGAL ? "LEGAL" :
                    r->plane_view == HDRPLAY_PLANE_CLIP ? "CLIP" :
                    r->plane_view == HDRPLAY_PLANE_PLATEAU ? "PLATEAU" : "COLOR";
                const char *sub =
                    r->diff_view
                        ? (r->plane_view == HDRPLAY_PLANE_Y  ? "ABS Y 4X" :
                           r->plane_view == HDRPLAY_PLANE_CB ? "ABS CB 4X" :
                           r->plane_view == HDRPLAY_PLANE_CR ? "ABS CR 4X" :
                           r->plane_view == HDRPLAY_PLANE_LEGAL ? "RED NOM MAX BLUE NOM MIN" :
                           r->plane_view == HDRPLAY_PLANE_CLIP ? "RED HIGH BLUE BLACK" :
                           r->plane_view == HDRPLAY_PLANE_PLATEAU ? "RED/BLUE FLAT END REGIONS" :
                                                              "ABS LINEAR 4X") :
                    r->plane_view == HDRPLAY_PLANE_Y ? "LUMA PLANE" :
                    r->plane_view == HDRPLAY_PLANE_COLOR ? "COMPOSITE" :
                    r->plane_view == HDRPLAY_PLANE_LEGAL ? "RED LEGAL WHITE BLUE BLACK" :
                    r->plane_view == HDRPLAY_PLANE_CLIP ? "RED CODE MAX BLUE CODE ZERO" :
                    r->plane_view == HDRPLAY_PLANE_PLATEAU ? "POSSIBLE CLIP: HIGH/LOW + FLAT" :
                                                           "CHROMA PLANE";
                if (build_label_badge(SLOT_PLANE_LABEL, gpu, big, sub,
                                      ov->dst) == 0) {
                    out->plane = overlay_arr[SLOT_PLANE_LABEL];
                    out->has_plane = true;
                }
                break;
            }

            case LAYOUT_OV_LABEL_A:
            case LAYOUT_OV_LABEL_B: {
                bool is_a = (ov->kind == LAYOUT_OV_LABEL_A);
                int  slot = is_a ? SLOT_HDR_LABEL : SLOT_SDR_LABEL;
                char big[16], sub[64];

                if (pair) {
                    /* Two files share one treatment, so the badge names
                     * the FILE. Which is what you need to know when the
                     * two panes look different. */
                    upper6(big, sizeof(big), sources[is_a ? ia : ib].label);
                    const Decoder *dec = &sources[is_a ? ia : ib].dec;
                    if (dec->has_ambient_viewing)
                        snprintf(sub, sizeof(sub), "AMVE %.0fLUX",
                                 dec->ambient_illuminance_lux);
                    else
                        snprintf(sub, sizeof(sub), "NO AMVE");
                } else {
                    /* Single file: the panes differ by TREATMENT. */
                    snprintf(big, sizeof(big), "%s", is_a ? "HDR" : "SDR");
                    if (is_a)
                        snprintf(sub, sizeof(sub), "BT.2020 PQ %.0fNITS",
                                 203.0f * r->display_hdr_headroom);
                    else
                        snprintf(sub, sizeof(sub), "TONEMAP %.0fNITS",
                                 r->sdr_peak_effective);
                }

                if (build_label_badge(slot, gpu, big, sub, ov->dst) == 0) {
                    if (is_a) { out->label_a = overlay_arr[slot]; out->has_label_a = true; }
                    else      { out->label_b = overlay_arr[slot]; out->has_label_b = true; }
                }
                break;
            }

            case LAYOUT_OV_INTERMEDIATE:
            case LAYOUT_OV_DIFF:
                break;   /* renderer owns these */
            }
        }
    }
}
