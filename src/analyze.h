#ifndef HDRPLAY_ANALYZE_H
#define HDRPLAY_ANALYZE_H

#include <stdbool.h>

struct SessionStats;
struct Decoder;

/* Headless whole-file content scan. Decodes every frame to EOF with no
 * SDL, no Vulkan and no window, so it runs over SSH and as fast as the
 * CPU allows.
 *
 *   stride      1 = every pixel (default; exact, and what makes the
 *               MaxCLL comparison sound). Larger values sample.
 *   hlg_lw      assumed HLG display peak in nits, <= 0 to take the
 *               file's mastering-display claim or fall back to 1000.
 *   json        emit machine-readable output on stdout in addition to
 *               the human report on stderr.
 *   stats_path  NDJSON per-frame series for plotting elsewhere (vca.py),
 *               or NULL.
 *
 * Returns the fail count, or a value >= HDRPLAY_EXIT_TOOL_ERROR when
 * the file could not be measured at all. */
int analyze_run(const char *path, int stride, double hlg_lw,
                bool json, const char *stats_path);

/* Print an accumulated session as a check report. Shared with playback
 * so the on-exit summary and the offline verdict read identically.
 *
 * The decoder is needed as well as the statistics: the declared
 * MaxCLL/MaxFALL and whether the colour range had to be recovered from
 * the pixels are properties of the source, not of the histogram, and
 * both change how the numbers below should be read. */
void analyze_print_session(const struct SessionStats *s,
                           const struct Decoder *dec,
                           const char *title);

#endif
