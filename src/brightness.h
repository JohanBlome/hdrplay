#ifndef HDRPLAY_BRIGHTNESS_H
#define HDRPLAY_BRIGHTNESS_H

/* Set screen brightness to `value` (0.0 .. 1.0) for the display at the
 * given SDL display index. Returns 0 on success, non-zero on failure
 * (including "no backend available for this display").
 *
 * Backend selection (first match wins):
 *   1. IOKit IODisplayConnect          — built-in Intel Macs, some externals
 *   2. shell `brightness` (Homebrew)   — built-in Apple Silicon Macs
 *   3. shell `m1ddc`                   — DDC/CI external displays
 *
 * Apple Pro Display XDR does NOT support any of the above — it requires
 * Apple's private CoreBrightness protocol and is settable only through
 * System Settings → Displays → Pro Display XDR → Brightness slider.
 */
int brightness_set(int display_index, float value);

#endif
