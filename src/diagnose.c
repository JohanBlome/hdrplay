#include "diagnose.h"
#include "checks.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <SDL3/SDL.h>

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>  /* CGDisplay* */
#include <IOKit/ps/IOPowerSources.h>                  /* battery     */
#include <IOKit/ps/IOPSKeys.h>
#endif

/* PASS/WARN/FAIL rendering and counting now live in checks.c, shared
 * with --analyze so both reports read identically and both use the
 * "exit code is the fail count" convention. */

/* ------------------------------------------------------------------ */
/* Read window-level HDR state by spawning a tiny hidden window on    */
/* the chosen display. SDL's per-display HDR header only carries the  */
/* on/off bit; SDR-white and headroom are only known once a window    */
/* has been placed on that display.                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    bool  hdr_enabled;
    float sdr_white;
    float headroom;
} DisplayHDRState;

static bool probe_display_hdr(SDL_DisplayID id, DisplayHDRState *out)
{
    memset(out, 0, sizeof(*out));

    SDL_PropertiesID wp = SDL_CreateProperties();
    SDL_SetStringProperty (wp, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "hdrplay-probe");
    SDL_SetNumberProperty (wp, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 64);
    SDL_SetNumberProperty (wp, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 64);
    SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    SDL_SetNumberProperty (wp, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                           SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_SetNumberProperty (wp, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                           SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_Window *w = SDL_CreateWindowWithProperties(wp);
    SDL_DestroyProperties(wp);
    if (!w) return false;

    /* SDL needs the window event pump to populate HDR state. */
    for (int i = 0; i < 8; i++) { SDL_PumpEvents(); SDL_Delay(15); }

    SDL_PropertiesID p = SDL_GetWindowProperties(w);
    out->hdr_enabled = SDL_GetBooleanProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    out->sdr_white   = SDL_GetFloatProperty  (p, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 0.0f);
    out->headroom    = SDL_GetFloatProperty  (p, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT,    1.0f);

    SDL_DestroyWindow(w);
    return true;
}

/* Heuristic reference-mode classifier. macOS doesn't publish the
 * preset name through any public API. SDL3 reports SDR white as a
 * normalized scaling factor (always 1.0 in HDR mode), not nits — so
 * the discriminating signal is the EDR headroom: HDR-tuned reference
 * modes leave lots of headroom (≥5×), Apple Display / SDR-heavy modes
 * leave very little. Pair this with the brightness slider position
 * (which we can't read) for a full picture. */
static const char *infer_reference_mode(float headroom)
{
    if (headroom < 1.1f)  return "no HDR overhead — SDR-only or `Apple Display` preset, or brightness near max";
    if (headroom < 2.0f)  return "limited HDR — mid-brightness or `Internet & Web` preset";
    if (headroom < 5.0f)  return "moderate HDR — typical XDR `Pro Display XDR (P3-1600)` mid-bright, or lower brightness";
    if (headroom < 10.0f) return "high HDR — XDR `HDR Video (P3-ST 2084)` or built-in at lower brightness";
    return                       "max HDR — display at minimum SDR brightness (best PQ accuracy)";
}

/* ------------------------------------------------------------------ */
/* macOS-specific corroborating checks                                 */
/* ------------------------------------------------------------------ */
#ifdef __APPLE__

/* Battery / Low Power Mode reduces brightness budget → reduces EDR
 * headroom on built-in panels. */
static void check_power_state(void)
{
    CFTypeRef snap = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = snap ? IOPSCopyPowerSourcesList(snap) : NULL;
    bool on_battery = false;
    int  pct = -1;
    if (list && CFArrayGetCount(list) > 0) {
        CFDictionaryRef ps = IOPSGetPowerSourceDescription(snap,
            CFArrayGetValueAtIndex(list, 0));
        if (ps) {
            CFStringRef state = CFDictionaryGetValue(ps, CFSTR(kIOPSPowerSourceStateKey));
            on_battery = state && CFEqual(state, CFSTR(kIOPSBatteryPowerValue));
            CFNumberRef cap = CFDictionaryGetValue(ps, CFSTR(kIOPSCurrentCapacityKey));
            if (cap) CFNumberGetValue(cap, kCFNumberIntType, &pct);
        }
    }
    if (list) CFRelease(list);
    if (snap) CFRelease(snap);

    char buf[128];
    if (on_battery) {
        snprintf(buf, sizeof(buf), "on battery @ %d%%  — built-in panels may cap brightness/EDR", pct);
        check(R_WARN, "power source", buf);
    } else {
        check(R_PASS, "power source", "AC power");
    }
}

/* Mirroring kills HDR signaling. Quartz tells us the truth even if SDL
 * lists the displays separately. */
static void check_mirroring(void)
{
    uint32_t count = 0;
    CGGetActiveDisplayList(0, NULL, &count);
    if (count == 0) { check(R_FAIL, "displays", "Quartz reports zero active displays"); return; }
    CGDirectDisplayID *ids = calloc(count, sizeof(*ids));
    CGGetActiveDisplayList(count, ids, &count);

    bool any_mirrored = false;
    for (uint32_t i = 0; i < count; i++) {
        if (CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) {
            any_mirrored = true;
            break;
        }
    }
    free(ids);
    if (any_mirrored)
        check(R_FAIL, "mirroring", "at least one display is mirroring another — HDR will be disabled");
    else
        check(R_PASS, "mirroring", "no displays are mirroring");
}

#else
static void check_power_state(void) { check(R_PASS, "power source", "(not checked on this OS)"); }
static void check_mirroring(void)   { check(R_PASS, "mirroring",    "(not checked on this OS)"); }
#endif

/* ------------------------------------------------------------------ */
/* Per-display checks                                                  */
/* ------------------------------------------------------------------ */
static void diagnose_one(int index, SDL_DisplayID id)
{
    const char *name = SDL_GetDisplayName(id) ?: "(unnamed)";
    fprintf(stderr, "\ndisplay [%d] %s\n", index, name);

    SDL_PropertiesID dp = SDL_GetDisplayProperties(id);
    bool display_hdr = SDL_GetBooleanProperty(dp, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    check(display_hdr ? R_PASS : R_FAIL, "display HDR mode",
          display_hdr ? "ON  (System Settings → Displays)" : "OFF — toggle HDR in System Settings");

    DisplayHDRState s;
    if (!probe_display_hdr(id, &s)) {
        check(R_FAIL, "probe window", "could not create probe window on this display");
        return;
    }

    char buf[160];
    snprintf(buf, sizeof(buf), "%s (window-level)",
             s.hdr_enabled ? "ON" : "OFF");
    check(s.hdr_enabled ? R_PASS : R_FAIL, "window HDR signaling", buf);

    /* SDL3 reports SDR white as a normalized scaling factor (1.0 = SDR
     * baseline), NOT in nits. To get nits, multiply by 100 (BT.2100 SDR
     * reference) or 203 (Apple's HDR Video reference). */
    snprintf(buf, sizeof(buf), "%.3f (normalized; ~%.0f nits assuming BT.2100 reference)",
             s.sdr_white, s.sdr_white * 100.0f);
    check(R_PASS, "SDR white scaling", buf);

    if (s.headroom >= 4.0f) {
        snprintf(buf, sizeof(buf), "%.2fx — plenty for PQ", s.headroom);
        check(R_PASS, "EDR headroom", buf);
    } else if (s.headroom >= 2.0f) {
        snprintf(buf, sizeof(buf), "%.2fx — usable, but limited", s.headroom);
        check(R_WARN, "EDR headroom", buf);
    } else if (s.headroom > 1.05f) {
        snprintf(buf, sizeof(buf), "%.2fx — too low for meaningful HDR", s.headroom);
        check(R_FAIL, "EDR headroom", buf);
    } else {
        check(R_FAIL, "EDR headroom", "≈1.0x — display is not delivering any HDR overhead");
    }

    snprintf(buf, sizeof(buf), "%s  (inferred from headroom)",
             infer_reference_mode(s.headroom));
    check(R_PASS, "reference mode", buf);
}

int diagnose_run(int display_index)
{
    check_reset();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    fprintf(stderr, "system checks\n");
    check_power_state();
    check_mirroring();

    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    if (!ids || count == 0) {
        check(R_FAIL, "displays", "SDL detected no displays");
        return 1;
    }

    if (display_index >= 0) {
        if (display_index >= count) {
            fprintf(stderr, "  display index %d out of range (have %d)\n", display_index, count);
            SDL_free(ids);
            return 1;
        }
        diagnose_one(display_index, ids[display_index]);
    } else {
        for (int i = 0; i < count; i++) diagnose_one(i, ids[i]);
    }
    SDL_free(ids);

    check_summary();
    return check_fail_count();
}
