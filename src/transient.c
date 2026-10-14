#include "transient.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

typedef struct {
    float *history;
    double sum, baseline, power, low_power, mid_power, high_power;
    double low, upper;
    float peak;
    int pos, filled, event_count, peak_at, clipped, attacks;
    bool was_low;
    int64_t onset, cooldown;
} Channel;
struct TransientDetector {
    int rate, channels, history_size, window, attack;
    int64_t sample;
    double alpha_low, alpha_upper;
    Channel *ch;
};

TransientDetector *transient_create(int rate, int channels)
{
    if (rate < 8000 || rate > 384000 || channels < 1 || channels > 8)
        return NULL;
    TransientDetector *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->rate = rate; d->channels = channels;
    d->history_size = rate / 20; /* 50 ms baseline */
    d->window = rate / 100;     /* 10 ms event */
    d->attack = rate / 500;     /* <= 2 ms attack */
    d->alpha_low = 1.0 - exp(-2.0 * 3.141592653589793 * 500 / rate);
    d->alpha_upper = 1.0 - exp(-2.0 * 3.141592653589793 * 3000 / rate);
    d->ch = calloc(channels, sizeof(*d->ch));
    if (!d->ch) { free(d); return NULL; }
    for (int c = 0; c < channels; c++) {
        d->ch[c].history = calloc(d->history_size, sizeof(float));
        if (!d->ch[c].history) { transient_destroy(d); return NULL; }
    }
    return d;
}

void transient_destroy(TransientDetector *d)
{
    if (!d) return;
    for (int c = 0; c < d->channels; c++) free(d->ch[c].history);
    free(d->ch); free(d);
}

void transient_reset(TransientDetector *d, int64_t sample)
{
    if (!d) return;
    d->sample = sample;
    for (int c = 0; c < d->channels; c++) {
        float *h = d->ch[c].history;
        memset(&d->ch[c], 0, sizeof(Channel));
        d->ch[c].history = h;
        memset(h, 0, d->history_size * sizeof(float));
    }
}

void transient_feed(TransientDetector *d, const float *pcm, int samples,
                    TransientCallback cb, void *userdata)
{
    if (!d || !pcm || samples <= 0) return;
    for (int i = 0; i < samples; i++, d->sample++) {
        for (int c = 0; c < d->channels; c++) {
            Channel *s = &d->ch[c];
            float x = pcm[i * d->channels + c];
            if (!isfinite(x)) x = 0;
            float mag = fabsf(x);
            double rms = sqrt(fmax(0, s->sum / d->history_size));
            if (!s->event_count && s->filled == d->history_size &&
                d->sample >= s->cooldown && mag >= 0.12589254f &&
                mag >= 10 * fmax(rms, 0.00001)) {
                /* Walk back over a rising attack in the ORIGINAL samples.
                 * Do not timestamp an envelope window or the peak. */
                int back = 0;
                double gate = fmax(0.0001, 5 * rms);
                for (int k = 1; k <= d->attack; k++) {
                    int p = (s->pos + d->history_size - k) % d->history_size;
                    if (fabsf(s->history[p]) <= gate) break;
                    back = k;
                }
                /* A quiet baseline is required, including exclusion of
                 * a previous attack in the baseline window. */
                float prior_peak = 0;
                for (int k = back + 1; k <= d->history_size; k++) {
                    int p = (s->pos + d->history_size - k) % d->history_size;
                    prior_peak = fmaxf(prior_peak, fabsf(s->history[p]));
                }
                if (back < d->attack && prior_peak < mag * 0.1f) {
                    s->onset = d->sample - back;
                    s->baseline = fmax(rms, 0.00001);
                    s->event_count = 1;
                    s->power = s->low_power = s->mid_power = s->high_power = 0;
                    s->low = s->upper = 0;
                    s->peak = 0; s->peak_at = 0; s->clipped = 0;
                    s->attacks = 1; s->was_low = false;
                }
            }
            if (s->event_count) {
                if (mag < fmax(0.0001, s->baseline * 5)) s->was_low = true;
                else if (s->was_low && s->peak > 0 && mag >= s->peak * 0.5f &&
                         d->sample - s->onset > d->attack) {
                    s->attacks++; s->was_low = false;
                }
                s->low += d->alpha_low * (x - s->low);
                s->upper += d->alpha_upper * (x - s->upper);
                double mid = s->upper - s->low, high = x - s->upper;
                s->power += x * x;
                s->low_power += s->low * s->low;
                s->mid_power += mid * mid;
                s->high_power += high * high;
                if (mag > s->peak) {
                    s->peak = mag;
                    s->peak_at = (int)(d->sample - s->onset);
                }
                if (mag >= 0.999f) s->clipped++;
                if (++s->event_count > d->window) {
                    double band_sum = s->low_power + s->mid_power + s->high_power;
                    double crest = s->peak * s->peak * d->window / fmax(s->power, 1e-20);
                    /* Broadband: substantial mid and high energy. Along
                     * with crest/attack gates this rejects tonal starts. */
                    bool broad = s->mid_power > band_sum * 0.08 &&
                                 s->high_power > band_sum * 0.12;
                    if (crest >= 15.8489319 && broad && s->peak_at <= d->attack &&
                        s->clipped < 4 && s->attacks == 1 && cb) {
                        TransientEvent event = {
                            .sample = s->onset, .channel = c, .peak = s->peak,
                            .contrast_db = (float)(20 * log10(s->peak / s->baseline)),
                            .uncertainty_samples = d->attack,
                        };
                        cb(userdata, &event);
                    }
                    s->event_count = 0;
                    /* One navigation marker per strike/echo cluster. */
                    s->cooldown = d->sample + d->rate / 5;
                }
            }
            float old = s->history[s->pos];
            s->sum = fmax(0, s->sum + (double)x * x - (double)old * old);
            s->history[s->pos] = x;
            s->pos = (s->pos + 1) % d->history_size;
            if (s->filled < d->history_size) s->filled++;
        }
    }
}
