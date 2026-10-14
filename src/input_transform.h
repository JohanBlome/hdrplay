#ifndef HDRPLAY_INPUT_TRANSFORM_H
#define HDRPLAY_INPUT_TRANSFORM_H

#include <math.h>
#include <stdbool.h>

/* Camera-Log interpretation applied after Y'CbCr -> nonlinear RGB. */
typedef enum {
    HDRPLAY_INPUT_TRANSFER_TAGGED = 0,
    HDRPLAY_INPUT_TRANSFER_ZLOG2,
    HDRPLAY_INPUT_TRANSFER_LOGC3,
} HdrplayInputTransfer;

typedef struct {
    HdrplayInputTransfer transfer;
    double exposure_stops; /* Z-Log2 calibration offset; ignored otherwise. */
} HdrplayInputTransform;

#define HDRPLAY_INPUT_TRANSFORM_TAGGED \
    ((HdrplayInputTransform){ HDRPLAY_INPUT_TRANSFER_TAGGED, 0.0 })

/* One set of constants feeds the CPU and, through stringification, the GLSL. */
#define HDRPLAY_ZLOG2_A   249.77914428710938
#define HDRPLAY_ZLOG2_B   0.9484496712684631
#define HDRPLAY_ZLOG2_C   0.3455176055431366
#define HDRPLAY_ZLOG2_D   0.1709400862455368
#define HDRPLAY_ZLOG2_E   1.3393715620040894
#define HDRPLAY_ZLOG2_F  -0.06127290427684784
#define HDRPLAY_ZLOG2_CUT -0.0652475655078888
#define HDRPLAY_ZLOG2_GAIN 13.5
/* Exposure bounds keep libplacebo's declared peak finite and inside its
 * 0..10,000-nit metadata domain. The upper bound is
 * log2(10000 / (203 * HDRPLAY_ZLOG2_GAIN)); the lower bound is a practical
 * calibration floor that still declares a non-zero 0.042-nit peak. */
#define HDRPLAY_ZLOG2_MIN_EXPOSURE -16.0
#define HDRPLAY_ZLOG2_MAX_EXPOSURE 1.8674889602

#define HDRPLAY_LOGC3_A   5.55555555555556
#define HDRPLAY_LOGC3_B   0.0522722750251688
#define HDRPLAY_LOGC3_C   0.247189638318671
#define HDRPLAY_LOGC3_D   0.385536998692443
#define HDRPLAY_LOGC3_CUT 0.0105909904954696
#define HDRPLAY_LOGC3_E   5.367655
#define HDRPLAY_LOGC3_F   0.092809
/* Preserve the complete 0..1 LogC3 signal inside libplacebo's 10,000-nit
 * color-space ceiling. This is a scene-relative exposure normalization
 * (-0.161 stops), not part of ARRI's normative LogC3 IDT. */
#define HDRPLAY_LOGC3_GAIN 0.8943544881299549

/* Matrix from the published Z CAM gamut -> ACES AP0 transform used by the
 * original implementation. It is intentionally not row-normalized: the
 * published transform is not white-preserving, and silently normalizing it
 * would change the authored color transform. */
#define HDRPLAY_ZCAM_AP0_00  0.59268922
#define HDRPLAY_ZCAM_AP0_01  0.23980554
#define HDRPLAY_ZCAM_AP0_02  0.09364286
#define HDRPLAY_ZCAM_AP0_10  0.00782530
#define HDRPLAY_ZCAM_AP0_11  0.99994013
#define HDRPLAY_ZCAM_AP0_12 -0.07973914
#define HDRPLAY_ZCAM_AP0_20  0.06103745
#define HDRPLAY_ZCAM_AP0_21 -0.24592234
#define HDRPLAY_ZCAM_AP0_22  1.13939635

#define HDRPLAY_AWG3_AP0_00  0.680205505106279
#define HDRPLAY_AWG3_AP0_01  0.236136601606481
#define HDRPLAY_AWG3_AP0_02  0.0836578932872398
#define HDRPLAY_AWG3_AP0_10  0.0854149797421404
#define HDRPLAY_AWG3_AP0_11  1.01747087860704
#define HDRPLAY_AWG3_AP0_12 -0.102885858349182
#define HDRPLAY_AWG3_AP0_20  0.00205652166929683
#define HDRPLAY_AWG3_AP0_21 -0.0625625003847921
#define HDRPLAY_AWG3_AP0_22  1.06050597871549

#define HDRPLAY_STRINGIFY_IMPL(x) #x
#define HDRPLAY_STRINGIFY(x) HDRPLAY_STRINGIFY_IMPL(x)

static inline bool hdrplay_input_is_log(const HdrplayInputTransform *t)
{
    return t && t->transfer != HDRPLAY_INPUT_TRANSFER_TAGGED;
}

static inline const char *hdrplay_input_transfer_name(HdrplayInputTransfer t)
{
    switch (t) {
    case HDRPLAY_INPUT_TRANSFER_ZLOG2: return "zlog2";
    case HDRPLAY_INPUT_TRANSFER_LOGC3: return "logc3";
    default: return "tagged";
    }
}

static inline double hdrplay_zlog2_decode(double encoded)
{
    /* The fit's cut is in the encoded domain. Clamp to the 0..1 domain of
     * the 4096-entry reference LUT so CPU and GPU handle YUV overshoot alike. */
    double t = fmin(fmax(encoded, 0.0), 1.0);
    double v = t > HDRPLAY_ZLOG2_CUT
        ? (pow(10.0, (t - HDRPLAY_ZLOG2_D) / HDRPLAY_ZLOG2_C) -
           HDRPLAY_ZLOG2_B) / HDRPLAY_ZLOG2_A
        : (t - HDRPLAY_ZLOG2_F) / HDRPLAY_ZLOG2_E;
    return fmax(v, 0.0);
}

static inline double hdrplay_logc3_decode(double encoded)
{
    double t = fmin(fmax(encoded, 0.0), 1.0);
    double encoded_cut = HDRPLAY_LOGC3_E * HDRPLAY_LOGC3_CUT + HDRPLAY_LOGC3_F;
    return t > encoded_cut
        ? (pow(10.0, (t - HDRPLAY_LOGC3_D) / HDRPLAY_LOGC3_C) -
           HDRPLAY_LOGC3_B) / HDRPLAY_LOGC3_A
        : (t - HDRPLAY_LOGC3_F) / HDRPLAY_LOGC3_E;
}

static inline double hdrplay_input_decode_channel(const HdrplayInputTransform *t,
                                                   double encoded)
{
    if (!t) return encoded;
    switch (t->transfer) {
    case HDRPLAY_INPUT_TRANSFER_ZLOG2:
        return hdrplay_zlog2_decode(encoded) * HDRPLAY_ZLOG2_GAIN *
               exp2(t->exposure_stops);
    case HDRPLAY_INPUT_TRANSFER_LOGC3:
        return fmax(hdrplay_logc3_decode(encoded), 0.0) *
               HDRPLAY_LOGC3_GAIN;
    default:
        return encoded;
    }
}

static inline void hdrplay_input_to_ap0(const HdrplayInputTransform *t,
                                        const double signal[3], double ap0[3])
{
    double v[3] = {
        hdrplay_input_decode_channel(t, signal[0]),
        hdrplay_input_decode_channel(t, signal[1]),
        hdrplay_input_decode_channel(t, signal[2]),
    };
    if (!t || t->transfer == HDRPLAY_INPUT_TRANSFER_TAGGED) {
        ap0[0] = v[0]; ap0[1] = v[1]; ap0[2] = v[2];
    } else if (t->transfer == HDRPLAY_INPUT_TRANSFER_ZLOG2) {
        ap0[0] = HDRPLAY_ZCAM_AP0_00*v[0] + HDRPLAY_ZCAM_AP0_01*v[1] + HDRPLAY_ZCAM_AP0_02*v[2];
        ap0[1] = HDRPLAY_ZCAM_AP0_10*v[0] + HDRPLAY_ZCAM_AP0_11*v[1] + HDRPLAY_ZCAM_AP0_12*v[2];
        ap0[2] = HDRPLAY_ZCAM_AP0_20*v[0] + HDRPLAY_ZCAM_AP0_21*v[1] + HDRPLAY_ZCAM_AP0_22*v[2];
    } else {
        ap0[0] = HDRPLAY_AWG3_AP0_00*v[0] + HDRPLAY_AWG3_AP0_01*v[1] + HDRPLAY_AWG3_AP0_02*v[2];
        ap0[1] = HDRPLAY_AWG3_AP0_10*v[0] + HDRPLAY_AWG3_AP0_11*v[1] + HDRPLAY_AWG3_AP0_12*v[2];
        ap0[2] = HDRPLAY_AWG3_AP0_20*v[0] + HDRPLAY_AWG3_AP0_21*v[1] + HDRPLAY_AWG3_AP0_22*v[2];
    }
}

/* libplacebo's PL_HDR_NORM convention maps linear 1.0 to 203 nits. Declare
 * the theoretical decoded signal peak rather than a lower display target;
 * tone mapping then owns the fit without clipping source highlights first. */
static inline double hdrplay_input_peak_nits(const HdrplayInputTransform *t)
{
    if (!hdrplay_input_is_log(t)) return 0.0;
    if (t->transfer == HDRPLAY_INPUT_TRANSFER_ZLOG2)
        return 203.0 * HDRPLAY_ZLOG2_GAIN * exp2(t->exposure_stops);
    return 10000.0;
}

#endif
