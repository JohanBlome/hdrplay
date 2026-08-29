#ifndef HDRPLAY_CHECKS_H
#define HDRPLAY_CHECKS_H

/* Shared PASS/WARN/FAIL reporting, lifted out of diagnose.c so
 * --diagnose (display state) and --analyze (content) render the same
 * way and share the "exit code is the fail count" convention.
 *
 * Everything goes to stderr, which leaves stdout free for --json. */

enum { R_PASS, R_WARN, R_FAIL, R_INFO };

void check_reset(void);
void check(int level, const char *name, const char *detail);
void check_note(const char *detail);          /* continuation line     */

int  check_fail_count(void);
int  check_warn_count(void);
void check_summary(void);                     /* "summary: N FAIL..."  */

/* Exit codes above this are tool errors (unreadable file, unsupported
 * format), not content verdicts. Without the separation a batch loop
 * cannot tell "2 FAILs" from "usage error", since main() already
 * returns 1 and 2 for those. */
#define HDRPLAY_EXIT_TOOL_ERROR 64

#endif
