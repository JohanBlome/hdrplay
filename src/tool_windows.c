#include "tool_windows.h"
#include "log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SDL's bundled font keeps these ordinary SDR windows independent of the
 * video's Vulkan surface. Coordinates are in window points, including HiDPI. */
enum { FONT = 2, LINE = 24, LIST_TOP = 208, ROW = 60 };
static void text(ToolPanel *p, float x, float y, const char *s)
{ SDL_RenderDebugText(p->canvas, x / FONT, y / FONT, s); }
static void color(ToolPanel *p, int r, int g, int b)
{ SDL_SetRenderDrawColor(p->canvas, r, g, b, 255); }
static void fill(ToolPanel *p, float x, float y, float w, float h)
{
    SDL_FRect rect = { x / FONT, y / FONT, w / FONT, h / FONT };
    SDL_RenderFillRect(p->canvas, &rect);
}
static int wrap(ToolPanel *p, int width, int x, int y, const char *s)
{
    int chars = (width - x - 24) / (8 * FONT);
    if (chars < 1) return y;
    while (*s) {
        int n = (int)strlen(s);
        if (n > chars) {
            n = chars;
            while (n > 0 && s[n] != ' ') n--;
            if (!n) n = chars;
        }
        char line[256];
        if (n > 255) n = 255;
        memcpy(line, s, n); line[n] = 0;
        text(p, x, y, line); y += LINE; s += n;
        while (*s == ' ') s++;
    }
    return y;
}
bool tool_windows_toggle(ToolWindows *tools, int kind)
{
    if (kind < 0 || kind >= TOOL_COUNT) return false;
    ToolPanel *p = &tools->panel[kind];
    if (!p->window) {
        p->window = SDL_CreateWindow(kind == TOOL_HELP ? "hdrplay - Keyboard help" :
                                    "hdrplay - Timing markers", 1060, 720,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (p->window) p->canvas = SDL_CreateRenderer(p->window, "software");
        if (!p->canvas) {
            LOG("UI", "cannot open tool window: %s", SDL_GetError());
            SDL_DestroyWindow(p->window); p->window = NULL; return false;
        }
        SDL_SetWindowMinimumSize(p->window, kind == TOOL_HELP ? 720 : 1060, 440);
    } else if (p->visible) {
        SDL_HideWindow(p->window); p->visible = false; return true;
    }
    SDL_ShowWindow(p->window); SDL_RaiseWindow(p->window);
    p->visible = p->dirty = true;
    return true;
}
typedef struct { const char *key, *action; } HelpRow;
static void help(ToolPanel *p, Renderer *r, int w, int h)
{
    const HelpRow rows[] = {
        {NULL, "WINDOWS"},
        {"F1", "Open / close keyboard help"},
        {"F3", "Open / close timing markers"},
        {"Esc / close", "Close this tool window; keep video open"},
        {"Wheel / PgUp / PgDn", "Scroll this window"},
        {NULL, "PLAYBACK (in the video window)"},
        {"Space", "Pause / resume"}, {"Comma / period", "Step backward / forward one frame; pause"},
        {"Left / Right", "Seek backward / forward 10 seconds"},
        {"L / R", "Toggle loop / restart from beginning"},
        {"Q / Esc", "Quit (in video window)"},
        {NULL, "VIDEO AND COMPARISON"},
        {"F", "Toggle fullscreen"}, {"H / S", "Select HDR / SDR"},
        {"P", "HDR/SDR split, or compare two inputs"},
        {"O", "Cycle pane layouts and comparison wipes"},
        {"D", "Toggle current/previous or A/B difference"},
        {"0 / 1 / 2", "Compare both / solo first / solo second input"},
        {"X", "Swap comparison sides"}, {"T", "Rotate focused pane 90 degrees clockwise"},
        {"Z", "Toggle fit / 1:1 source pixels"}, {"+ / -", "Zoom in / out"},
        {"Drag / Shift-arrows", "Pan while zoomed"}, {"W", "Resize video window for exact 1:1"},
        {NULL, "VIDEO INSPECTION"},
        {r->audio_visible ? "Shift-C" : "C", "Cycle color / Y / Cb / Cr / legal / clip / plateau"},
        {"V", "Cycle waveform / gamut / vector / histogram scopes"},
        {"Shift-drag", "Select scope source area; Shift-click clears it"},
        {r->audio_visible ? "Shift-G" : "G", "Toggle vectorscope trace gain 1x / 2x"},
        {"M", "Toggle mouse luminance probe"}, {"I", "Show / hide status HUD"},
        {"A / Shift-A", "Show / hide accumulated stats / reset stats"},
        {NULL, "AUDIO WAVEFORM (requires --audio-waveform)"},
        {"F4 / scale link", "Toggle linear / log amplitude; original samples retained"},
        {"Wheel", "Zoom time around pointer, down to source samples"},
        {"Drag / Shift-wheel", "Pan waveform"}, {"Shift-H / Home", "Restore waveform follow"},
        {"Ruler click", "Seek and pause"}, {"U", "Mute / unmute; retain audio clock"},
        {"B", "Mark displayed video frame; make it the reference"},
        {"C / right-click", "Mark source sample at cursor / clicked time"},
        {"Shift-click", "Add audio sample marker"},
        {"Marker click", "Select audio marker; video marker becomes reference"},
        {"Ctrl-click marker", "Use any marker as reference"},
        {"[ / ]", "Previous / next detected transient; seek and pause"},
        {"Enter", "Accept selected marker"}, {"Shift-Enter", "Adjust selected marker to cursor sample"},
        {"Delete / Backspace", "Dismiss selected marker"},
        {"G", "Enter absolute media time; Enter confirms, Esc cancels"},
        {"F2", "Label selected marker in video window"},
        {"Ctrl-S / Cmd-S", "Save --markers FILE"},
        {NULL, "TIMING MARKERS WINDOW"},
        {"Click row", "Select marker; video rows also set the reference"},
        {"Ctrl / Cmd-click", "Make any row the reference"},
        {"Double-click / J", "Seek and pause at selected marker"},
        {"Up / Down", "Select previous / next row"}, {"A", "Make selected row the reference"},
        {"Enter / Delete", "Accept / dismiss selected row"},
        {"Ctrl-S / Cmd-S", "Save --markers FILE"},
    };
    color(p, 233, 240, 250); text(p, 24, 20, "Keyboard help");
    color(p, 160, 180, 200); text(p, 24, 52, "Video and audio controls. Scroll for all sections.");
    SDL_Rect clip = {0, 80 / FONT, w / FONT, (h - 116) / FONT};
    SDL_SetRenderClipRect(p->canvas, &clip);
    int y = 92 - p->scroll;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (!rows[i].key) {
            y += 12; color(p, 90, 205, 210); text(p, 24, y, rows[i].action); y += 34;
        } else {
            color(p, 235, 240, 250); text(p, 24, y, rows[i].key);
            color(p, 185, 200, 218); y = wrap(p, w, 360, y, rows[i].action) + 8;
        }
    }
    int content = y + p->scroll - 92;
    int max = content > h - 128 ? content - (h - 128) : 0;
    if (p->scroll > max) { p->scroll = max; p->dirty = true; }
    SDL_SetRenderClipRect(p->canvas, NULL);
    color(p, 160, 180, 200); text(p, 24, h - 28, "F1 or Esc closes help. Playback continues.");
}
static const char *kind_name(const AudioMarker *m)
{
    return m->kind == AUDIO_MARK_FRAME ? "Video" : m->kind == AUDIO_MARK_TIME ? "Time" :
           m->kind == AUDIO_MARK_TRANSIENT ? "Transient" : "Sample";
}
static void markers(ToolPanel *p, Renderer *r, int w, int h)
{
    color(p, 233, 240, 250); text(p, 24, 20, "Timing markers");
    if (!r->audio) {
        color(p, 185, 200, 218);
        wrap(p, w, 24, 68, "Enable --audio-waveform to measure timing and create markers.");
        return;
    }
    AudioMarker *m = malloc(AUDIO_MAX_MARKERS * sizeof(*m));
    if (!m) return;
    int n = audio_markers(r->audio, m, AUDIO_MAX_MARKERS);
    AudioInfo info; audio_info(r->audio, &info);
    int a = r->audio_view.anchor, s = r->audio_view.selected;
    bool anchor = a >= 0 && a < n && !m[a].dismissed;
    char line[256];
    if (anchor) snprintf(line, sizeof(line), "Reference #%d: %s at %.6f s", a + 1, kind_name(&m[a]), m[a].time);
    else snprintf(line, sizeof(line), "Select a video row, or Ctrl/Cmd-click any row to set reference.");
    color(p, 100, 210, 225); wrap(p, w, 24, 54, line);
    if (anchor && s >= 0 && s < n && !m[s].dismissed) {
        double d = m[s].time - m[a].time;
        snprintf(line, sizeof(line), "#%d delta %+.3f ms / %+.3f sample intervals", s + 1, d * 1000, d * info.rate);
        color(p, 245, 218, 140); text(p, 24, 86, line);
        if (m[a].kind == AUDIO_MARK_FRAME && m[a].end > m[a].time)
            snprintf(line, sizeof(line), "Frame range [%+.3f, %+.3f] ms; uncertainty %.3f ms",
                     (m[s].time - m[a].end - m[s].uncertainty) * 1000,
                     (d + m[s].uncertainty) * 1000, m[s].uncertainty * 1000);
        else snprintf(line, sizeof(line), "Marker timing uncertainty %.3f ms", m[s].uncertainty * 1000);
        color(p, 185, 200, 218); wrap(p, w, 24, 114, line);
    } else {
        color(p, 185, 200, 218); text(p, 24, 90, "Click a row to compare its time with the reference.");
    }
    color(p, 160, 180, 200); text(p, 24, 154, "[A Reference]  [J Seek]  [Enter Accept]  [Del Dismiss]");
    snprintf(line, sizeof(line), "%4s %-10s %12s  %12s  %s", "#", "Kind", "Time (s)", "Delta (ms)", "State / label");
    text(p, 24, 180, line);
    SDL_Rect clip = {0, LIST_TOP / FONT, w / FONT, (h - LIST_TOP - 36) / FONT};
    SDL_SetRenderClipRect(p->canvas, &clip);
    int max_scroll = n * ROW - (h - LIST_TOP - 36);
    if (max_scroll < 0) max_scroll = 0;
    if (p->scroll > max_scroll) p->scroll = max_scroll;
    for (int i = 0; i < n; i++) {
        int y = LIST_TOP + i * ROW - p->scroll;
        if (y + ROW < LIST_TOP || y >= h - 36) continue;
        color(p, i == s ? 30 : 18, i == s ? 55 : 25, i == s ? 76 : 34);
        fill(p, 16, y, w - 32, ROW - 2);
        color(p, m[i].dismissed ? 115 : i == a ? 100 : 230,
                 m[i].dismissed ? 125 : i == a ? 210 : 240,
                 m[i].dismissed ? 135 : i == a ? 225 : 250);
        char delta[32] = "--";
        if (anchor) snprintf(delta, sizeof(delta), "%+.3f", (m[i].time - m[a].time) * 1000);
        snprintf(line, sizeof(line), "%4d %-10s %12.6f  %12s  %s%s", i + 1,
                 kind_name(&m[i]), m[i].time, delta,
                 m[i].dismissed ? "Dismissed" : m[i].reviewed ? "Reviewed" : "Candidate",
                 i == a ? " / REF" : "");
        text(p, 24, y + 6, line);
        if (m[i].kind == AUDIO_MARK_FRAME)
            snprintf(line, sizeof(line), "  PTS %lld (%d/%d)  input %d  %s", (long long)m[i].pts,
                     m[i].tb.num, m[i].tb.den, m[i].input, m[i].label);
        else if (m[i].channel >= 0)
            snprintf(line, sizeof(line), "  Sample %lld  channel %d  %s", (long long)m[i].sample, m[i].channel + 1, m[i].label);
        else snprintf(line, sizeof(line), "  %s", m[i].label);
        /* Ellipsis preserves table alignment when a label is too long. */
        int cap = (w - 48) / (8 * FONT);
        if (cap >= 3 && cap < (int)strlen(line)) { line[cap] = 0; memcpy(line + cap - 3, "...", 3); }
        color(p, 160, 180, 200); text(p, 24, y + 32, line);
    }
    if (!n) { color(p, 185, 200, 218); text(p, 24, LIST_TOP + 12, "No markers yet. Press B in video to mark a frame."); }
    SDL_SetRenderClipRect(p->canvas, NULL);
    color(p, 160, 180, 200); text(p, 24, h - 28, "Delta = row time - reference time. F1 help. Ctrl/Cmd-S save.");
    free(m);
}
static int marker_action(Renderer *r, int action, double *seek)
{
    int s = r->audio_view.selected;
    AudioMarker *m = malloc(AUDIO_MAX_MARKERS * sizeof(*m));
    if (!m) return AUDIO_UI_HANDLED;
    int n = audio_markers(r->audio, m, AUDIO_MAX_MARKERS);
    int result = AUDIO_UI_HANDLED;
    if (s >= 0 && s < n) {
        if (action == 0 && !m[s].dismissed) r->audio_view.anchor = s;
        if (action == 1) { *seek = fmax(0, m[s].time); result |= AUDIO_UI_SEEK; }
        if (action == 2) audio_marker_review(r->audio, s, false);
        if (action == 3) audio_marker_review(r->audio, s, true);
    }
    free(m); return result;
}
int tool_windows_event(ToolWindows *tools, Renderer *r, Source *sources,
                       const SDL_Event *e, double *seek)
{
    SDL_Window *event_window = SDL_GetWindowFromEvent(e);
    SDL_WindowID id = event_window ? SDL_GetWindowID(event_window) : 0;
    bool main = id == SDL_GetWindowID(r->window);
    if (e->type == SDL_EVENT_KEY_DOWN && !r->audio_view.entry_mode &&
        (e->key.key == SDLK_F1 || e->key.key == SDLK_F3)) {
        if (!e->key.repeat) tool_windows_toggle(tools, e->key.key == SDLK_F1 ? TOOL_HELP : TOOL_MARKERS);
        return AUDIO_UI_HANDLED;
    }
    if (main && r->audio_visible && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        e->button.button == SDL_BUTTON_LEFT) {
        AudioView *v = &r->audio_view;
        int lw, lh; SDL_GetWindowSize(r->window, &lw, &lh);
        if (lw > 0 && lh > 0 && v->win_w > 0 && v->win_h > 0) {
            double x = e->button.x * v->win_w / lw - v->rect.x0;
            double y = e->button.y * v->win_h / lh - v->rect.y0;
            int w = v->rect.x1 - v->rect.x0, h = v->rect.y1 - v->rect.y0;
            int scale = w >= 1100 && h >= 240 ? 2 : 1;
            int footer = 34 * scale;
            AudioInfo info; audio_info(r->audio, &info);
            if (h - 68 * scale < info.channels * 4) footer = 24;
            if (x >= 0 && y >= h - footer && y < h - footer + 14 * scale) {
                if (x < 80 * scale) tool_windows_toggle(tools, TOOL_HELP);
                else if (x < 188 * scale) tool_windows_toggle(tools, TOOL_MARKERS);
                else if (x < 310 * scale) v->logarithmic = !v->logarithmic;
                else return 0;
                return AUDIO_UI_HANDLED;
            }
        }
    }
    for (int k = 0; k < TOOL_COUNT; k++) {
        ToolPanel *p = &tools->panel[k];
        if (!p->window || id != SDL_GetWindowID(p->window)) continue;
        p->dirty = true;
        if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
            (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE)) {
            SDL_HideWindow(p->window); p->visible = false; return AUDIO_UI_HANDLED;
        }
        int w, h; SDL_GetWindowSize(p->window, &w, &h);
        if (e->type == SDL_EVENT_MOUSE_WHEEL) p->scroll = (int)fmax(0, p->scroll - e->wheel.y * 3 * LINE);
        if (e->type == SDL_EVENT_KEY_DOWN) {
            SDL_Keycode key = e->key.key;
            if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN)
                p->scroll = (int)fmax(0, p->scroll + (key == SDLK_PAGEUP ? -1 : 1) * (h - 160));
            if (k == TOOL_HELP && (key == SDLK_UP || key == SDLK_DOWN))
                p->scroll = (int)fmax(0, p->scroll + (key == SDLK_UP ? -1 : 1) * LINE);
            if (k == TOOL_MARKERS && r->audio) {
                if (key == SDLK_UP || key == SDLK_DOWN) {
                    AudioInfo info; audio_info(r->audio, &info);
                    int s = r->audio_view.selected + (key == SDLK_UP ? -1 : 1);
                    if (s < 0) s = 0;
                    if (s >= info.markers) s = info.markers - 1;
                    r->audio_view.selected = s;
                    if (s >= 0) {
                        if (s * ROW < p->scroll) p->scroll = s * ROW;
                        if ((s + 1) * ROW > p->scroll + h - LIST_TOP - 36)
                            p->scroll = (s + 1) * ROW - (h - LIST_TOP - 36);
                        AudioMarker *m = malloc(AUDIO_MAX_MARKERS * sizeof(*m));
                        if (m) {
                            int n = audio_markers(r->audio, m, AUDIO_MAX_MARKERS);
                            if (s < n) {
                                r->audio_view.cursor = m[s].time;
                                if (m[s].channel >= 0) r->audio_view.channel = m[s].channel;
                                r->audio_view.follow = false;
                                r->audio_view.start = fmax(0, m[s].time - r->audio_view.span / 2);
                            }
                            free(m);
                        }
                    }
                }
                if (key == SDLK_A) return marker_action(r, 0, seek);
                if (key == SDLK_J) return marker_action(r, 1, seek);
                if (key == SDLK_RETURN) return marker_action(r, 2, seek);
                if (key == SDLK_DELETE || key == SDLK_BACKSPACE) return marker_action(r, 3, seek);
                if (key == SDLK_S && (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)))
                    if (!audio_markers_save(r->audio)) LOG("AUDIO", "save failed; specify --markers FILE");
            }
        }
        if (k == TOOL_MARKERS && r->audio && e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
            if (e->button.y >= LIST_TOP && e->button.y < h - 36) {
                int i = ((int)e->button.y - LIST_TOP + p->scroll) / ROW;
                AudioMarker *m = malloc(AUDIO_MAX_MARKERS * sizeof(*m));
                if (m) {
                    int n = audio_markers(r->audio, m, AUDIO_MAX_MARKERS);
                    if (i >= 0 && i < n) {
                        r->audio_view.selected = i; r->audio_view.cursor = m[i].time;
                        if (m[i].channel >= 0) r->audio_view.channel = m[i].channel;
                        r->audio_view.follow = false;
                        r->audio_view.start = fmax(0, m[i].time - r->audio_view.span / 2);
                        if (!m[i].dismissed && (m[i].kind == AUDIO_MARK_FRAME ||
                            (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI)))) r->audio_view.anchor = i;
                    }
                    free(m);
                    if (i >= 0 && i < n && e->button.clicks >= 2) return marker_action(r, 1, seek);
                }
            } else if (e->button.y >= 148 && e->button.y < 176 && e->button.x >= 24) {
                float x = e->button.x;
                if (x < 232) return marker_action(r, 0, seek);
                if (x < 392) return marker_action(r, 1, seek);
                if (x < 648) return marker_action(r, 2, seek);
                if (x < 888) return marker_action(r, 3, seek);
            }
        }
        return AUDIO_UI_HANDLED;
    }
    return 0;
}
void tool_windows_draw(ToolWindows *tools, Renderer *r)
{
    AudioInfo info = {0}; if (r->audio) audio_info(r->audio, &info);
    bool changed = tools->revision != info.revision || tools->selected != r->audio_view.selected ||
                   tools->anchor != r->audio_view.anchor || tools->audio_visible != r->audio_visible ||
                   tools->logarithmic != r->audio_view.logarithmic;
    tools->revision = info.revision; tools->selected = r->audio_view.selected;
    tools->anchor = r->audio_view.anchor; tools->audio_visible = r->audio_visible;
    tools->logarithmic = r->audio_view.logarithmic;
    for (int k = 0; k < TOOL_COUNT; k++) {
        ToolPanel *p = &tools->panel[k];
        if (!p->visible || (!p->dirty && !changed)) continue;
        p->dirty = false;
        int w, h, pw, ph;
        SDL_GetWindowSize(p->window, &w, &h); SDL_GetWindowSizeInPixels(p->window, &pw, &ph);
        if (w <= 0 || h <= 0) continue;
        SDL_SetRenderScale(p->canvas, FONT * (float)pw / w, FONT * (float)ph / h);
        color(p, 12, 18, 26); SDL_RenderClear(p->canvas);
        if (k == TOOL_HELP) help(p, r, w, h); else markers(p, r, w, h);
        SDL_RenderPresent(p->canvas);
    }
}
void tool_windows_close(ToolWindows *tools)
{
    for (int k = 0; k < TOOL_COUNT; k++) {
        SDL_DestroyRenderer(tools->panel[k].canvas);
        SDL_DestroyWindow(tools->panel[k].window);
    }
    memset(tools, 0, sizeof(*tools));
}
