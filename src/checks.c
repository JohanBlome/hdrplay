#include "checks.h"

#include <stdio.h>
#include <unistd.h>

static int fail_count = 0;
static int warn_count = 0;

void check_reset(void) { fail_count = warn_count = 0; }
int  check_fail_count(void) { return fail_count; }
int  check_warn_count(void) { return warn_count; }

void check(int level, const char *name, const char *detail)
{
    /* Colour only when stderr is a terminal. Redirecting the report to
     * a file or piping it into grep otherwise embeds escape codes in
     * every line, which breaks both reading and matching. */
    static const char *tag_ansi[] = {
        "\x1b[32mPASS\x1b[0m",
        "\x1b[33mWARN\x1b[0m",
        "\x1b[31mFAIL\x1b[0m",
        "\x1b[36mINFO\x1b[0m",
    };
    static const char *tag_plain[] = { "PASS", "WARN", "FAIL", "INFO" };
    const char **tag = isatty(fileno(stderr)) ? tag_ansi : tag_plain;
    fprintf(stderr, "  %s  %-32s  %s\n", tag[level], name, detail);
    if (level == R_FAIL) fail_count++;
    if (level == R_WARN) warn_count++;
}

void check_note(const char *detail)
{
    fprintf(stderr, "        %-32s  %s\n", "", detail);
}

void check_summary(void)
{
    fprintf(stderr, "\nsummary: %d FAIL, %d WARN\n", fail_count, warn_count);
}
