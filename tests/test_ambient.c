#include <math.h>
#include <stdio.h>

#include "ambient.h"

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    CHECK(fabsf(ambient_contrast_exponent(314.0f, 314.0f) - 1.0f) < 1e-6f,
          "reference environment is neutral");
    CHECK(ambient_contrast_exponent(314.0f, 15.0f) > 1.3f,
          "dark room lowers midtones");
    CHECK(ambient_contrast_exponent(314.0f, 1000.0f) < 1.0f,
          "bright room lifts midtones");
    CHECK(ambient_contrast_exponent(314.0f, 0.001f) == 1.4f,
          "dark-room expansion is capped");
    CHECK(ambient_contrast_exponent(0.0f, 15.0f) == 1.0f,
          "missing reference disables adjustment");
    return failures ? 1 : 0;
}
