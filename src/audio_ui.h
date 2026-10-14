#ifndef HDRPLAY_AUDIO_UI_H
#define HDRPLAY_AUDIO_UI_H
#include "renderer.h"
#include "source.h"
enum { AUDIO_UI_HANDLED = 1, AUDIO_UI_SEEK = 2 };
int audio_ui_event(Renderer *r, Source *sources, const SDL_Event *event,
                    double *seek_time);
#endif
