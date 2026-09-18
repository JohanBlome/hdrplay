#include "ambient.h"

#include <math.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

float ambient_contrast_exponent(float reference_lux, float viewing_lux)
{
    if (!(reference_lux > 0.0f) || !(viewing_lux > 0.0f) ||
        !isfinite(reference_lux) || !isfinite(viewing_lux))
        return 1.0f;

    float exponent = 1.0f + 0.08f * log2f(reference_lux / viewing_lux);
    if (exponent < 0.75f) exponent = 0.75f;
    if (exponent > 1.40f) exponent = 1.40f;
    return exponent;
}

bool ambient_sensor_read_lux(float *lux)
{
    if (lux) *lux = 0.0f;
#ifdef __APPLE__
    /* CurrentLux and this service class are Apple implementation details,
     * not a documented ambient-light API. Keep failure harmless and expose
     * --ambient-lux as the portable/manual path. */
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("AppleSPUVD6286"));
    if (!service) return false;

    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, CFSTR("CurrentLux"), kCFAllocatorDefault, 0);
    IOObjectRelease(service);
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) {
        if (value) CFRelease(value);
        return false;
    }

    double measured = 0.0;
    bool ok = CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType,
                               &measured);
    CFRelease(value);
    if (!ok || !isfinite(measured) || measured < 0.0) return false;
    /* Zero lux is a valid dark-room reading, but the logarithmic policy
     * needs a positive floor and will clamp to its maximum expansion. */
    if (lux) *lux = (float)fmax(measured, 0.1);
    return true;
#else
    return false;
#endif
}
