#include "audio_ui.h"
#include "tool_windows.h"
#include "waveform_scale.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

int g_verbose;
static int fails;
#define CHECK(c, name) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", name); fails++; } } while (0)
static void u16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void u32(FILE *f, unsigned v) { u16(f, v); u16(f, v >> 16); }
static void wav(const char *path)
{
    FILE *f = fopen(path, "wb"); if (!f) exit(1);
    fwrite("RIFF", 1, 4, f); u32(f, 96000 + 36); fwrite("WAVEfmt ", 1, 8, f);
    u32(f, 16); u16(f, 1); u16(f, 1); u32(f, 48000); u32(f, 96000);
    u16(f, 2); u16(f, 16); fwrite("data", 1, 4, f); u32(f, 96000);
    for (int i = 0; i < 48000; i++) u16(f, i == 12000 ? 16384 : 0);
    fclose(f);
}
static int key(Renderer *r, Source *s, SDL_Keycode code, SDL_Keymod mod, double *seek)
{
    SDL_Event e = { .type = SDL_EVENT_KEY_DOWN };
    e.key.key = code; e.key.mod = mod;
    return audio_ui_event(r, s, &e, seek);
}
int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    char file[] = "/tmp/hdrplay-ui-test-XXXXXX", sidecar[] = "/tmp/hdrplay-ui-markers-XXXXXX";
    int fd = mkstemp(file); if (fd < 0) return 1; close(fd); wav(file);
    fd = mkstemp(sidecar); if (fd < 0) return 1; close(fd); unlink(sidecar);
    const char *paths[] = {file};
    Audio *a = audio_open(paths, 1, 0, 0, false, true, sidecar);
    if (!a) return 1;
    AudioInfo info; uint64_t deadline = SDL_GetTicks() + 5000;
    do { audio_info(a, &info); if (info.ready) break; SDL_Delay(5); } while (SDL_GetTicks() < deadline);
    CHECK(info.ready && info.transients == 1, "UI fixture decoded with automatic marker");
    Renderer r = { .audio = a, .audio_visible = true, .n_sources = 1, .solo = -1 };
    r.window = SDL_CreateWindow("audio-ui-test", 960, 600, 0);
    r.audio_view = (AudioView){ .span = 2, .selected = -1, .anchor = -1,
        .rect = layout_audio_rect(1920, 1200), .win_w = 1920, .win_h = 1200 };
    Source s = { .tb_sec = .001, .fps = 25, .timeline_origin = 2 };
    s.dec.fmt = avformat_alloc_context();
    AVStream *stream = avformat_new_stream(s.dec.fmt, NULL);
    stream->time_base = (AVRational){1,1000};
    s.shown = av_frame_alloc(); s.shown->best_effort_timestamp = 2240;
    s.pending = av_frame_alloc(); s.pending->best_effort_timestamp = 2280;
    double seek = 0;
    CHECK(key(&r, &s, SDLK_B, 0, &seek) == AUDIO_UI_HANDLED, "frame marker key handled");
    AudioMarker markers[16]; int n = audio_markers(a, markers, 16);
    CHECK(n == 2 && markers[1].pts == 2240 && fabs(markers[1].time - .24) < 1e-9 &&
          fabs(markers[1].end - .28) < 1e-9, "frame marker retains original PTS and frame interval");
    SDL_Event e = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN };
    e.button.button = SDL_BUTTON_RIGHT; e.button.x = 120; e.button.y = 450;
    CHECK(audio_ui_event(&r, &s, &e, &seek) == AUDIO_UI_HANDLED, "waveform intercepts pointer");
    n = audio_markers(a, markers, 16);
    CHECK(n == 3 && markers[2].sample == 12000 && markers[2].time == .25,
          "HiDPI waveform click maps to exact source sample");
    key(&r, &s, SDLK_F2, 0, &seek);
    e = (SDL_Event){ .type = SDL_EVENT_TEXT_INPUT }; e.text.text = "CLAP";
    audio_ui_event(&r, &s, &e, &seek); key(&r, &s, SDLK_RETURN, 0, &seek);
    n = audio_markers(a, markers, 16);
    CHECK(!strcmp(markers[2].label, "CLAP"), "text entry labels selected marker");
    key(&r, &s, SDLK_G, 0, &seek); e.text.text = "0.42";
    audio_ui_event(&r, &s, &e, &seek); key(&r, &s, SDLK_RETURN, 0, &seek);
    n = audio_markers(a, markers, 16);
    CHECK(n == 4 && markers[3].kind == AUDIO_MARK_TIME && markers[3].time == .42, "absolute media time entry");
    r.audio_view.cursor = .3;
    r.audio_view.follow = false;
    CHECK(key(&r, &s, SDLK_H, SDL_KMOD_SHIFT, &seek) == AUDIO_UI_HANDLED && r.audio_view.follow,
          "Shift-H restores waveform follow on laptop keyboards");
    CHECK(key(&r, &s, SDLK_H, 0, &seek) == 0, "bare H remains available for HDR mode");
    CHECK(key(&r, &s, SDLK_C, SDL_KMOD_SHIFT, &seek) == 0 &&
          key(&r, &s, SDLK_G, SDL_KMOD_SHIFT, &seek) == 0,
          "video planes and vector gain remain accessible with waveform enabled");
    CHECK(key(&r, &s, SDLK_F4, 0, &seek) == AUDIO_UI_HANDLED && r.audio_view.logarithmic,
          "F4 enables log amplitude");
    CHECK(waveform_amplitude(0, true) == 0 && waveform_amplitude(1, true) == 1 &&
          waveform_amplitude(-1, true) == -1, "log view retains silence and signed full scale");
    CHECK(waveform_amplitude(.001f, true) > .09f &&
          waveform_amplitude(.001f, true) < waveform_amplitude(.01f, true) &&
          waveform_amplitude(-.001f, true) == -waveform_amplitude(.001f, true),
          "log view expands quiet amplitudes monotonically and retains polarity");
    CHECK(waveform_amplitude(.001f, false) == .001f, "linear amplitude retains original scale");
    CHECK(key(&r, &s, SDLK_LEFTBRACKET, 0, &seek) == (AUDIO_UI_HANDLED | AUDIO_UI_SEEK) &&
          seek == .25 && r.audio_view.selected == 0, "transient navigation requests pause/seek at onset");
    key(&r, &s, SDLK_RETURN, 0, &seek);
    r.audio_view.cursor = .26;
    key(&r, &s, SDLK_RETURN, SDL_KMOD_SHIFT, &seek);
    n = audio_markers(a, markers, 16);
    CHECK(markers[0].reviewed && markers[0].sample == 12480 && markers[0].detected_sample == 12000,
          "manual adjustment retains original detection identity");
    ToolWindows tools = {0};
    CHECK(tool_windows_toggle(&tools, TOOL_HELP), "help opens as separate SDL window");
    CHECK(tool_windows_toggle(&tools, TOOL_MARKERS), "marker list opens as separate SDL window");
    tool_windows_draw(&tools, &r);
    SDL_WindowID help_id = SDL_GetWindowID(tools.panel[TOOL_HELP].window);
    SDL_WindowID list_id = SDL_GetWindowID(tools.panel[TOOL_MARKERS].window);
    CHECK(help_id && list_id && help_id != list_id && list_id != SDL_GetWindowID(r.window),
          "tool windows have independent event identities");
    e = (SDL_Event){ .type = SDL_EVENT_MOUSE_BUTTON_DOWN };
    e.button.windowID = list_id; e.button.button = SDL_BUTTON_LEFT;
    e.button.x = 60; e.button.y = 208 + 60 + 10;
    CHECK(tool_windows_event(&tools, &r, &s, &e, &seek) == AUDIO_UI_HANDLED &&
          r.audio_view.selected == 1 && r.audio_view.anchor == 1,
          "video row selection sets comparison reference");
    e.button.y = 208 + 120 + 10; e.button.clicks = 2;
    CHECK(tool_windows_event(&tools, &r, &s, &e, &seek) == (AUDIO_UI_HANDLED | AUDIO_UI_SEEK) &&
          r.audio_view.selected == 2 && seek == .25 && r.audio_view.anchor == 1,
          "double click sample row seeks and retains video reference");
    n = audio_markers(a, markers, 16);
    CHECK(fabs((markers[2].time - markers[r.audio_view.anchor].time) * 1000 - 10) < 1e-8,
          "row comparison retains sample and frame time precision");
    e = (SDL_Event){ .type = SDL_EVENT_KEY_DOWN };
    e.key.windowID = list_id; e.key.key = SDLK_DOWN;
    tool_windows_event(&tools, &r, &s, &e, &seek);
    CHECK(r.audio_view.selected == 3 && r.audio_view.cursor == .42 && r.audio_view.anchor == 1,
          "keyboard row selection updates waveform cursor and retains reference");
    e.key.key = SDLK_UP; tool_windows_event(&tools, &r, &s, &e, &seek);
    e.key.key = SDLK_DELETE;
    tool_windows_event(&tools, &r, &s, &e, &seek);
    n = audio_markers(a, markers, 16);
    CHECK(markers[2].dismissed, "list dismiss action updates shared marker state");
    e.key.key = SDLK_RETURN; tool_windows_event(&tools, &r, &s, &e, &seek);
    e.key.windowID = help_id; e.key.key = SDLK_SPACE;
    CHECK(tool_windows_event(&tools, &r, &s, &e, &seek) == AUDIO_UI_HANDLED,
          "help key events cannot change playback transport");
    e.key.key = SDLK_ESCAPE;
    CHECK(tool_windows_event(&tools, &r, &s, &e, &seek) == AUDIO_UI_HANDLED &&
          !tools.panel[TOOL_HELP].visible && tools.panel[TOOL_MARKERS].visible,
          "Escape closes only the focused help window");
    tool_windows_draw(&tools, &r);
    tool_windows_close(&tools);
    key(&r, &s, SDLK_S, SDL_KMOD_CTRL, &seek);
    SDL_DestroyWindow(r.window); source_close(&s); audio_close(a);
    a = audio_open(paths, 1, 0, 0, false, true, sidecar);
    CHECK(a != NULL, "UI marker sidecar reloads");
    if (a) {
        deadline = SDL_GetTicks() + 5000;
        do { audio_info(a, &info); if (info.ready) break; SDL_Delay(5); } while (SDL_GetTicks() < deadline);
        n = audio_markers(a, markers, 16);
        CHECK(n == 4 && markers[0].sample == 12480, "reanalysis preserves adjusted transient without duplicates");
        audio_close(a);
    }
    unlink(file); unlink(sidecar); SDL_Quit();
    printf("audio UI: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
