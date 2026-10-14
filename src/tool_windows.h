#ifndef HDRPLAY_TOOL_WINDOWS_H
#define HDRPLAY_TOOL_WINDOWS_H
#include "audio_ui.h"

enum { TOOL_HELP, TOOL_MARKERS, TOOL_COUNT };
typedef struct {
    SDL_Window *window;
    SDL_Renderer *canvas;
    bool visible, dirty;
    int scroll;
} ToolPanel;
typedef struct {
    ToolPanel panel[TOOL_COUNT];
    uint64_t revision;
    int selected, anchor;
    bool audio_visible, logarithmic;
} ToolWindows;

bool tool_windows_toggle(ToolWindows *tools, int kind);
/* Auxiliary window events never reach the video window's controls. */
int tool_windows_event(ToolWindows *tools, Renderer *r, Source *sources,
                       const SDL_Event *event, double *seek);
void tool_windows_draw(ToolWindows *tools, Renderer *r);
void tool_windows_close(ToolWindows *tools);
#endif
