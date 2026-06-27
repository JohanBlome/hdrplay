#include "brightness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <SDL3/SDL.h>

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>  /* CGDirectDisplayID */
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#endif

static const char *display_name_at(int idx)
{
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    const char *name = (ids && idx >= 0 && idx < count) ? SDL_GetDisplayName(ids[idx]) : NULL;
    SDL_free(ids);
    return name;
}

/* ------------------------------------------------------------------ */
/* Backend 1 — IOKit IODisplayConnect (Intel Macs, some externals)    */
/* ------------------------------------------------------------------ */
#ifdef __APPLE__
static int try_iokit(float value)
{
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(0 /* kIOMainPortDefault */,
            IOServiceMatching("IODisplayConnect"), &iter) != KERN_SUCCESS)
        return -1;

    io_object_t svc;
    int hits = 0;
    while ((svc = IOIteratorNext(iter))) {
        kern_return_t kr = IODisplaySetFloatParameter(svc, kNilOptions,
            CFSTR(kIODisplayBrightnessKey), value);
        if (kr == KERN_SUCCESS) hits++;
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return hits > 0 ? 0 : -1;
}

#else
static int try_iokit(float v)  { (void)v; return -1; }
#endif

/* ------------------------------------------------------------------ */
/* Backend 2/3 — shell out to Homebrew tools if installed             */
/* ------------------------------------------------------------------ */
static bool tool_exists(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

static int try_brightness_cli(float value)
{
    if (!tool_exists("brightness")) return -1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "brightness %.3f >/dev/null 2>&1", value);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

/* m1ddc takes 0-100, and DDC/CI VCP code 0x10 (luminance/brightness)
 * is what you want. Works on USB-C-connected DDC-compliant displays. */
static int try_m1ddc(float value)
{
    if (!tool_exists("m1ddc")) return -1;
    int v100 = (int)(value * 100.0f + 0.5f);
    if (v100 < 0) v100 = 0;
    if (v100 > 100) v100 = 100;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "m1ddc set luminance %d >/dev/null 2>&1", v100);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
int brightness_set(int display_index, float value)
{
    if (value < 0.0f || value > 1.0f) {
        LOG("HDR", "brightness value %.3f out of range [0..1]", value);
        return 2;
    }
    const char *name = display_name_at(display_index);
    if (name && strcasestr(name, "XDR")) {
        LOG("HDR", "Pro Display XDR detected on [%d] %s — no public API can",
            display_index, name);
        LOG("HDR", "set brightness on this panel. Open System Settings →");
        LOG("HDR", "Displays → %s → Brightness, or change the preset.", name);
        return 3;
    }

    bool is_builtin = name && (strcasestr(name, "Built-in") || strcasestr(name, "Retina"));

    /* Built-in first: IOKit, then Homebrew `brightness`. */
    if (is_builtin) {
        if (try_iokit(value) == 0) {
            LOG("HDR", "brightness set via IOKit on [%d] %s → %.2f", display_index, name, value); return 0;
        }
        if (try_brightness_cli(value) == 0) {
            LOG("HDR", "brightness set via `brightness` CLI on [%d] %s → %.2f", display_index, name, value); return 0;
        }
        LOG("HDR", "no backend could set brightness on [%d] %s.", display_index, name);
        LOG("HDR", "On Apple Silicon: brew install brightness");
        return 1;
    }

    /* External: m1ddc first (DDC/CI), then IOKit as long-shot. */
    if (try_m1ddc(value) == 0) {
        LOG("HDR", "brightness set via m1ddc on [%d] %s → %.2f", display_index, name ?: "(unnamed)", value);
        return 0;
    }
    if (try_iokit(value) == 0) {
        LOG("HDR", "brightness set via IOKit on [%d] %s → %.2f", display_index, name ?: "(unnamed)", value);
        return 0;
    }
    LOG("HDR", "no backend could set brightness on [%d] %s.", display_index, name ?: "(unnamed)");
    LOG("HDR", "For DDC/CI external displays: brew install m1ddc");
    LOG("HDR", "Some panels (incl. Pro Display XDR) have no programmatic brightness API.");
    return 1;
}
