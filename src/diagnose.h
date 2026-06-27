#ifndef HDRPLAY_DIAGNOSE_H
#define HDRPLAY_DIAGNOSE_H

/* Run the HDR sanity check suite against display `display_index`, or all
 * displays if display_index < 0. Prints a per-check PASS/WARN/FAIL grid
 * to stderr. Returns the number of FAIL checks across all displays — use
 * as the process exit code so a wrapper script can refuse to run a test
 * suite when the panel state is wrong. */
int diagnose_run(int display_index);

#endif
