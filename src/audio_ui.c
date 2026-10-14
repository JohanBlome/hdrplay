#include "audio_ui.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

static int count(Audio *a)
{ AudioInfo i; audio_info(a, &i); return i.markers; }

static void sample_marker(Renderer *r)
{
    AudioView *v = &r->audio_view;
    if (audio_marker_sample(r->audio, v->cursor, v->channel))
        v->selected = count(r->audio) - 1;
    else LOG("AUDIO", "no decoded audio sample at %.6f s", v->cursor);
}

static void frame_marker(Renderer *r, Source *s)
{
    int focus = renderer_focus_source(r);
    s += focus;
    if (!s->shown) return;
    double t = source_shown_sec(s), end = source_peek_next_sec(s);
    if (!isfinite(end) || end <= t) end = t + (s->fps > 0 ? 1 / s->fps : 0);
    if (audio_marker_frame(r->audio, focus, s->dec.stream_idx,
                           s->shown->best_effort_timestamp,
                           s->dec.fmt->streams[s->dec.stream_idx]->time_base, t, end)) {
        r->audio_view.anchor = count(r->audio) - 1;
        LOG("AUDIO", "video marker at %.6f s (PTS %lld)", t,
             (long long)s->shown->best_effort_timestamp);
    } else LOG("AUDIO", "cannot mark a frame without a presentation timestamp");
}

int audio_ui_event(Renderer *r, Source *sources, const SDL_Event *e,
                    double *seek_time)
{
    if (!r->audio_visible || !r->audio) return 0;
    AudioView *v = &r->audio_view;
    if (v->entry_mode) {
        if (e->type == SDL_EVENT_TEXT_INPUT) {
            size_t n = strlen(v->entry), len = strlen(e->text.text);
            if (n + len < sizeof(v->entry)) memcpy(v->entry + n, e->text.text, len + 1);
            return AUDIO_UI_HANDLED;
        }
        if (e->type == SDL_EVENT_KEY_DOWN) {
            if (e->key.key == SDLK_ESCAPE) { v->entry_mode = 0; SDL_StopTextInput(r->window); }
            else if (e->key.key == SDLK_BACKSPACE) {
                size_t n = strlen(v->entry);
                if (n) { do { n--; } while (n && ((unsigned char)v->entry[n] & 0xc0) == 0x80); v->entry[n] = 0; }
            } else if (e->key.key == SDLK_RETURN) {
                bool ok = false;
                if (v->entry_mode == 1) {
                    char *end; errno = 0;
                    double t = strtod(v->entry, &end);
                    ok = !errno && end != v->entry && !*end && isfinite(t) && t >= 0 &&
                         audio_marker_time(r->audio, t);
                    if (ok) { v->cursor = t; v->selected = count(r->audio) - 1; }
                } else ok = audio_marker_label(r->audio, v->selected, v->entry);
                if (ok) { v->entry_mode = 0; SDL_StopTextInput(r->window); }
                else LOG("AUDIO", "invalid marker entry");
            }
            return AUDIO_UI_HANDLED;
        }
    }
    if (e->type == SDL_EVENT_KEY_DOWN) {
        bool shift = (e->key.mod & SDL_KMOD_SHIFT) != 0;
        if ((e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && e->key.key == SDLK_S) {
            if (!audio_markers_save(r->audio)) LOG("AUDIO", "save failed; specify --markers FILE to persist measurements");
            return AUDIO_UI_HANDLED;
        }
        switch (e->key.key) {
        case SDLK_B: if (!e->key.repeat) frame_marker(r, sources); return AUDIO_UI_HANDLED;
        case SDLK_C:
            if (shift) break; /* Keep video plane cycling available. */
            if (!e->key.repeat) sample_marker(r);
            return AUDIO_UI_HANDLED;
        case SDLK_F4:
            if (!e->key.repeat) v->logarithmic = !v->logarithmic;
            return AUDIO_UI_HANDLED;
        case SDLK_HOME: v->follow = true; return AUDIO_UI_HANDLED;
        case SDLK_H:
            if (shift) { v->follow = true; return AUDIO_UI_HANDLED; }
            break;
        case SDLK_U: {
            r->audio_muted = !r->audio_muted; audio_mute(r->audio, r->audio_muted);
            LOG("AUDIO", "%s", r->audio_muted ? "muted" : "unmuted");
            return AUDIO_UI_HANDLED;
        }
        case SDLK_G: case SDLK_F2:
            if (e->key.key == SDLK_G && shift) break;
            if (e->key.key == SDLK_F2 && v->selected < 0) return AUDIO_UI_HANDLED;
            v->entry_mode = e->key.key == SDLK_G ? 1 : 2;
            v->entry[0] = 0; SDL_StartTextInput(r->window);
            return AUDIO_UI_HANDLED;
        case SDLK_RETURN:
            if (shift) audio_marker_move(r->audio, v->selected, v->cursor, v->channel);
            else audio_marker_review(r->audio, v->selected, false);
            return AUDIO_UI_HANDLED;
        case SDLK_DELETE: case SDLK_BACKSPACE:
            audio_marker_review(r->audio, v->selected, true); return AUDIO_UI_HANDLED;
        case SDLK_LEFTBRACKET: case SDLK_RIGHTBRACKET: {
            AudioMarker m; int index;
            if (audio_transient_near(r->audio, v->cursor,
                                      e->key.key == SDLK_RIGHTBRACKET ? 1 : -1, &m, &index)) {
                v->selected = index; v->cursor = m.time; v->channel = m.channel;
                v->follow = false; v->start = fmax(0, m.time - v->span / 2);
                *seek_time = fmax(0, m.time);
                return AUDIO_UI_HANDLED | AUDIO_UI_SEEK;
            }
            return AUDIO_UI_HANDLED;
        }
        default: break;
        }
    }
    double wx = 0, wy = 0;
    bool pointer = true;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        wx = e->button.x; wy = e->button.y;
    } else if (e->type == SDL_EVENT_MOUSE_MOTION) { wx = e->motion.x; wy = e->motion.y; }
    else if (e->type == SDL_EVENT_MOUSE_WHEEL) { wx = e->wheel.mouse_x; wy = e->wheel.mouse_y; }
    else pointer = false;
    if (!pointer || v->win_w <= 0 || v->win_h <= 0) return 0;
    int lw, lh; SDL_GetWindowSize(r->window, &lw, &lh);
    if (lw <= 0 || lh <= 0) return 0;
    wx *= (double)v->win_w / lw; wy *= (double)v->win_h / lh;
    bool inside = wx >= v->rect.x0 && wx < v->rect.x1 && wy >= v->rect.y0 && wy < v->rect.y1;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && v->dragging) { v->dragging = false; return AUDIO_UI_HANDLED; }
    if (!inside && !v->dragging) return 0;
    double fraction = (wx - v->rect.x0) / (v->rect.x1 - v->rect.x0);
    double time = v->start + fraction * v->span;
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        if (shift || fabs(e->wheel.x) > fabs(e->wheel.y))
            v->start = fmax(0, v->start - (e->wheel.x + e->wheel.y) * v->span * 0.1);
        else {
            AudioInfo info; audio_info(r->audio, &info);
            double new_span = v->span * pow(1.35, -e->wheel.y);
            new_span = fmax(4.0 / info.rate, fmin(36000, new_span));
            v->start = fmax(0, time - fraction * new_span); v->span = new_span;
        }
        v->follow = false;
    } else if (e->type == SDL_EVENT_MOUSE_MOTION) {
        if (v->dragging) {
            v->start = fmax(0, v->start - (wx - v->drag_x) * v->span / (v->rect.x1 - v->rect.x0));
            v->drag_x = wx; v->follow = false;
        } else v->cursor = time;
    } else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        AudioInfo info; audio_info(r->audio, &info);
        int w = (int)(v->rect.x1 - v->rect.x0), h = (int)(v->rect.y1 - v->rect.y0);
        int scale = w >= 1100 && h >= 240 ? 2 : 1;
        int header = 34 * scale, footer = 46 * scale;
        if (h - header - footer < info.channels * 4) { header = 30; footer = 36; }
        int y = (int)(wy - v->rect.y0);
        v->cursor = time;
        if (y < header && e->button.button == SDL_BUTTON_LEFT) {
            *seek_time = fmax(0, time); return AUDIO_UI_HANDLED | AUDIO_UI_SEEK;
        }
        if (y >= h - footer) return AUDIO_UI_HANDLED;
        v->channel = (y - header) * info.channels / (h - header - footer);
        if (v->channel < 0) v->channel = 0;
        if (v->channel >= info.channels) v->channel = info.channels - 1;
        if (e->button.button == SDL_BUTTON_RIGHT || (SDL_GetModState() & SDL_KMOD_SHIFT)) sample_marker(r);
        else {
            AudioMarker *m = malloc(AUDIO_MAX_MARKERS * sizeof(*m));
            if (m) {
                int n = audio_markers(r->audio, m, AUDIO_MAX_MARKERS), best = -1;
                double distance = 7 * v->span / w;
                for (int i = 0; i < n; i++) {
                    if (m[i].dismissed || (m[i].channel >= 0 && m[i].channel != v->channel)) continue;
                    double d = fabs(time - m[i].time);
                    if (d <= distance) { best = i; distance = d; }
                }
                if (best >= 0) {
                    if (m[best].kind == AUDIO_MARK_FRAME || (SDL_GetModState() & SDL_KMOD_CTRL)) v->anchor = best;
                    else v->selected = best;
                    v->cursor = m[best].time;
                }
                free(m);
            }
            v->dragging = true; v->drag_x = wx;
        }
    }
    return AUDIO_UI_HANDLED;
}
