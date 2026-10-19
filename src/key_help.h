#ifndef HDRPLAY_KEY_HELP_H
#define HDRPLAY_KEY_HELP_H

#include <stddef.h>

/* Shared by CLI and F1 help so shortcuts and their focus scopes agree. */
typedef struct {
    const char *key;
    const char *action;
    const char *scope; /* section rows have no key */
} HdrplayHelpRow;

static const HdrplayHelpRow hdrplay_key_help[] = {
    {NULL, "WINDOWS", "F1 / F3: any window. Other keys: tool windows"},
    {"F1", "Open / close keyboard help", NULL},
    {"F3", "Open / close timing markers", NULL},
    {"Esc / close", "Close this tool window; keep video open", NULL},
    {"Wheel / PgUp / PgDn", "Scroll this window", NULL},
    {NULL, "PLAYBACK", "Video window"},
    {"Space", "Pause / resume", NULL}, {"Comma / period", "Step backward / forward one frame; pause", NULL},
    {"Left / Right", "Seek backward / forward 10 seconds", NULL},
    {"L / R", "Toggle loop / restart from beginning", NULL},
    {"Q / Esc", "Quit (in video window)", NULL},
    {NULL, "COMPARISON", "Video window"},
    {"H / S", "Select HDR / SDR", NULL},
    {"P", "HDR/SDR split, or compare two inputs", NULL},
    {"O", "Cycle pane layouts and comparison wipes", NULL},
    {"D", "Toggle current/previous or A/B difference", NULL},
    {"0 / 1 / 2", "Compare both / solo first / solo second input", NULL},
    {"X", "Swap comparison sides", NULL},
    {NULL, "IMAGE VIEW", "Video window"},
    {"F", "Toggle fullscreen", NULL}, {"T", "Rotate focused pane 90 degrees clockwise", NULL},
    {"Z", "Toggle fit / 1:1 source pixels", NULL}, {"+ / -", "Zoom in / out", NULL},
    {"Drag / Shift-arrows", "Pan while zoomed", NULL}, {"W", "Resize video window for exact 1:1", NULL},
    {NULL, "SCOPES AND INSPECTION", "Video window; Shift-C / Shift-G with audio strip"},
    {"C / Shift-C", "Cycle color / Y / Cb / Cr / legal / clip / plateau", NULL},
    {"V", "Cycle waveform / gamut / vector / histogram scopes", NULL},
    {"Shift-drag", "Select scope source area; Shift-click clears it", NULL},
    {"G / Shift-G", "Toggle vectorscope trace gain 1x / 2x", NULL},
    {"M", "Toggle mouse luminance probe", NULL}, {"I", "Show / hide status HUD", NULL},
    {"A / Shift-A", "Show / hide accumulated stats / reset stats", NULL},
    {NULL, "AUDIO WAVEFORM", "Video window with --audio-waveform enabled"},
    {"F4 / scale link", "Toggle linear / log amplitude; original samples retained", NULL},
    {"Wheel", "Zoom time around pointer, down to source samples", NULL},
    {"Drag / Shift-wheel", "Pan waveform", NULL}, {"Shift-H / Home", "Restore waveform follow", NULL},
    {"Ruler click", "Seek and pause", NULL}, {"U", "Mute / unmute; retain audio clock", NULL},
    {NULL, "AUDIO MARKERS", "Video window with --audio-waveform enabled"},
    {"B", "Mark displayed video frame; make it the reference", NULL},
    {"C / right-click", "Mark source sample at cursor / clicked time", NULL},
    {"Shift-click", "Add audio sample marker", NULL},
    {"Marker click", "Select audio marker; video marker becomes reference", NULL},
    {"Ctrl-click marker", "Use any marker as reference", NULL},
    {"[ / ]", "Previous / next detected transient; seek and pause", NULL},
    {"Enter", "Accept selected marker", NULL}, {"Shift-Enter", "Adjust selected marker to cursor sample", NULL},
    {"Delete / Backspace", "Dismiss selected marker", NULL},
    {"G", "Enter absolute media time; Enter confirms, Esc cancels", NULL},
    {"F2", "Label selected marker in video window", NULL},
    {"Ctrl-S / Cmd-S", "Save --markers FILE", NULL},
    {NULL, "TIMING MARKERS", "Timing markers window (F3)"},
    {"Click row", "Select marker; video rows also set the reference", NULL},
    {"Ctrl / Cmd-click", "Make any row the reference", NULL},
    {"Double-click / J", "Seek and pause at selected marker", NULL},
    {"Up / Down", "Select previous / next row", NULL}, {"A", "Make selected row the reference", NULL},
    {"Enter / Delete", "Accept / dismiss selected row", NULL},
    {"Ctrl-S / Cmd-S", "Save --markers FILE", NULL},
};

#define HDRPLAY_KEY_HELP_COUNT (sizeof(hdrplay_key_help) / sizeof(hdrplay_key_help[0]))

#endif
