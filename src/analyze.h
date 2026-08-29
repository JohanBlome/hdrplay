#ifndef HDRPLAY_ANALYZE_H
#define HDRPLAY_ANALYZE_H

#include <stdbool.h>

struct SessionStats;

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
 * declared_cll_max / _avg are -1 when the container declares none. */
void analyze_print_session(const struct SessionStats *s,
                           int declared_cll_max, int declared_cll_avg,
                           const char *title);

#endif
