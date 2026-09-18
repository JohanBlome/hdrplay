#ifndef HDRPLAY_AMBIENT_H
#define HDRPLAY_AMBIENT_H

#include <stdbool.h>

/* hdrplay viewing-environment policy. This is deliberately not labelled an
 * AMVE/BT.2100 conformance transform: those specifications describe the
 * metadata but do not prescribe this mapping.
 *
 * Every stop by which the room is darker than the mastering/reference room
 * adds 0.08 to a display-linear luma exponent. The result is clamped so bad
 * sensor values cannot make the image unusable. Black and peak remain fixed. */
float ambient_contrast_exponent(float reference_lux, float viewing_lux);

/* Best-effort built-in ambient light sensor. Currently implemented for the
 * Apple-silicon sensor service used by modern MacBooks. */
bool ambient_sensor_read_lux(float *lux);

#endif
