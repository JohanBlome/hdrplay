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
enum { SLOT_STATUS, SLOT_HDR_LABEL, SLOT_SDR_LABEL, SLOT_SESSION, SLOT_COUNT };

typedef struct { pl_tex tex; int W, H; } HudSlot;

static HudSlot      slots[SLOT_COUNT];
static int          hud_scale = 2;

/* Storage for overlay descriptors passed to libplacebo each frame.
 * Sized once, mutated per-frame. */
static struct pl_overlay      overlay_arr[SLOT_COUNT];
static struct pl_overlay_part overlay_parts[SLOT_COUNT];

void hud_close(pl_gpu gpu)
{
    for (int s = 0; s < SLOT_COUNT; s++) {
        if (slots[s].tex) pl_tex_destroy(gpu, &slots[s].tex);
        slots[s].W = slots[s].H = 0;
    }
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
    /* H must fit the worst case, which is SPLIT mode with the probe on:
     * MODE, DISPLAY HDR, src csp, out csp, FRAME, SRC PEAK, DR, SDR CAP,
     * ABOVE 500N, SDR BOOST, PROBE, RGB = 12 lines, plus the excessive-
     * headroom warning = 13. Line n starts at 6 + 24(n-1) and is 16px
     * tall, so 13 lines needs 6 + 24*12 + 16 = 310. At the old H = 220
     * anything past line 9 was silently dropped by draw_text_color's
     * bounds check — pressing M in split mode looked like a no-op
     * because both PROBE lines fell off the bottom. */
    const int W = 460, H = 340;
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

    snprintf(line, sizeof(line), "DR %.1f STOPS (P99.9/P1)", d.dr_stops);
    draw_text(buf, W, H, 6, y, hud_scale, line); y += pitch;

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
                    snprintf(sub, sizeof(sub), "%s %.0fNITS",
                             r->mode == HDRPLAY_MODE_SDR ? "SDR" : "HDR",
                             r->mode == HDRPLAY_MODE_SDR
                               ? r->sdr_peak_effective
                               : 203.0f * r->display_hdr_headroom);
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
                break;   /* renderer owns these */
            }
        }
    }
}
