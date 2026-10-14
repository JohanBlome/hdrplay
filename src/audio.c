#include "audio_internal.h"
#include "media_time.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/intreadwrite.h>

static double wall(void) { return SDL_GetTicksNS() / 1e9; }
static int cancelled(void *opaque)
{ return SDL_GetAtomicInt(&((Audio *)opaque)->stop); }

static void file_audit_locked(Audio *a)
{
    AudioInput *in = &a->inputs[a->input];
    if (a->file_audit_complete || !a->finished || a->failed || !in->file_end_known ||
        in->other_tracks || (in->stream >= 0 && (!in->complete || !in->n_frames))) return;
    double end = a->duration;
    if (in->n_frames) {
        AudioFrame *last = &in->frames[in->n_frames - 1];
        if (last->end <= last->time) return; /* final frame duration unknown */
        end = fmax(end, last->end);
    }
    a->file_observed_end = end;
    a->file_duration_delta = end - in->declared_file_end;
    a->file_duration_mismatch = fabs(a->file_duration_delta) > fmax(.001, a->timestamp_uncertainty);
    a->file_audit_complete = true;
    LOG("AUDIO", "%s: file duration %.6f s; decoded media endpoint %.6f s, difference %+.3f ms",
         a->file_duration_mismatch ? "WARN" : "INFO", in->declared_file_duration,
         end + (in->declared_file_duration - in->declared_file_end), a->file_duration_delta * 1000);
    a->revision++;
}

static void fail(Audio *a, const char *message)
{
    SDL_LockMutex(a->mutex);
    if (!a->failed) snprintf(a->error, sizeof(a->error), "%s", message);
    a->failed = true;
    a->revision++;
    SDL_BroadcastCondition(a->changed);
    SDL_UnlockMutex(a->mutex);
    LOG("AUDIO", "%s", a->error);
}

static bool reserve(void **ptr, int *cap, int n, size_t size, int limit)
{
    if (n > limit) return false;
    if (n <= *cap) return true;
    int c = *cap ? *cap * 2 : 256;
    if (c < n) c = n;
    if (c > limit) c = limit;
    void *p = realloc(*ptr, (size_t)c * size);
    if (!p) return false;
    *ptr = p; *cap = c;
    return true;
}

static bool peak_add(Audio *a, AudioPeak peak, int level)
{
    AudioPeaks *p = &a->peaks[level];
    if (!reserve((void **)&p->p, &p->cap, p->n + 1, sizeof(*p->p),
                 AUDIO_PEAK_LIMIT)) return false;
    if (!level) peak.first = peak.last = p->n;
    p->p[p->n++] = peak;
    if (p->n % 4 || level + 1 == AUDIO_PEAK_LEVELS) return true;
    AudioPeak sum = p->p[p->n - 4];
    for (int i = p->n - 3; i < p->n; i++) {
        /* Never merge across absent audio. Reset grouping at a gap by
         * leaving these base bins for the finer-level query. */
        if (fabs(p->p[i].time - sum.end) > 0.5 / a->rate) return true;
        sum.end = p->p[i].end;
        sum.last = p->p[i].last;
        for (int c = 0; c < a->channels; c++) {
            sum.low[c] = fminf(sum.low[c], p->p[i].low[c]);
            sum.high[c] = fmaxf(sum.high[c], p->p[i].high[c]);
        }
    }
    return peak_add(a, sum, level + 1);
}

static int block_at(Audio *a, double time)
{
    int lo = 0, hi = a->n_blocks;
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        if (a->blocks[m].end <= time + 1e-10) lo = m + 1;
        else hi = m;
    }
    return lo;
}

bool audio_append_marker(Audio *a, const AudioMarker *marker)
{
    if (a->n_markers >= AUDIO_MAX_MARKERS) return false;
    a->markers[a->n_markers++] = *marker;
    a->revision++;
    return true;
}

static void detected(void *opaque, const TransientEvent *e)
{
    Audio *a = opaque;
    SDL_LockMutex(a->mutex);
    /* Preserve individual channel arrival times; navigation merges them. */
    for (int i = 0; i < a->n_markers; i++) {
        AudioMarker *m = &a->markers[i];
        if (m->kind == AUDIO_MARK_TRANSIENT && m->detected_sample == e->sample &&
            m->detected_channel == e->channel) { SDL_UnlockMutex(a->mutex); return; }
    }
    for (int i = a->n_blocks - 1; i >= 0; i--) {
        AudioBlock *b = &a->blocks[i];
        if (e->sample < b->sample || e->sample >= b->sample + b->count) continue;
        AudioMarker m = {
            .kind = AUDIO_MARK_TRANSIENT, .input = a->input,
            .stream = a->stream, .channel = e->channel, .frame = -1,
            .sample = e->sample, .pts = b->pts, .tb = b->tb,
            .detected_sample = e->sample, .detected_channel = e->channel,
            .time = b->time + (double)(e->sample - b->sample) / a->rate,
            .uncertainty = (double)e->uncertainty_samples / a->rate + a->timestamp_uncertainty,
            .timestamp_uncertainty = a->timestamp_uncertainty,
            .peak = e->peak, .contrast_db = e->contrast_db,
        };
        snprintf(m.label, sizeof(m.label), "TRANSIENT CH%d", e->channel + 1);
        audio_append_marker(a, &m);
        LOGV("AUDIO", "transient %.6f s, channel %d, sample %lld, contrast %.1f dB",
             m.time, e->channel + 1, (long long)e->sample, e->contrast_db);
        break;
    }
    SDL_UnlockMutex(a->mutex);
}

static bool store_frame(Audio *a, AVFrame *f, TransientDetector *detector)
{
    if (f->sample_rate != a->rate || f->ch_layout.nb_channels != a->channels ||
        f->format != a->cc->sample_fmt) {
        fail(a, "audio format changed; sample timing cannot be preserved");
        return false;
    }
    if (f->best_effort_timestamp == AV_NOPTS_VALUE) {
        fail(a, "audio frame has no timestamp; precise sync is unavailable");
        return false;
    }
    AVRational tb = a->fmt->streams[a->stream]->time_base;
    double t = f->best_effort_timestamp * av_q2d(tb) - a->inputs[a->input].origin;
    int count = f->nb_samples;
    if (count <= 0) return true;
    int64_t bytes = (int64_t)count * a->channels * sizeof(float);
    if (bytes > AUDIO_CACHE_LIMIT - a->bytes) {
        fail(a, "audio cache limit (4 GiB) reached; use a shorter inspection clip");
        return false;
    }
    float *pcm = av_malloc((size_t)bytes);
    if (!pcm) { fail(a, "audio PCM allocation failed"); return false; }
    uint8_t *out[] = { (uint8_t *)pcm };
    int n = swr_convert(a->swr, out, count, (const uint8_t **)f->extended_data, count);
    if (n != count) {
        av_free(pcm); fail(a, "unexpected PCM conversion delay"); return false;
    }
    /* Native rate planar -> packed float conversion has no resampling delay.
     * FFmpeg's default audio decoder already applies skip-sample side data. */
    if (pwrite(a->fd, pcm, (size_t)bytes, a->bytes) != bytes) {
        av_free(pcm); fail(a, "cannot write temporary audio cache"); return false;
    }
    bool gap = false, ok = true;
    const char *failure = "audio peak/index cache limit reached";
    SDL_LockMutex(a->mutex);
    if (a->n_blocks) {
        double end = a->blocks[a->n_blocks - 1].end;
        /* Matroska commonly timestamps audio in milliseconds. Within half
         * a timestamp tick, continue the ORIGINAL sample clock instead of
         * manufacturing gaps/overlaps from timestamp rounding. */
        double tolerance = fmax(1.5 / a->rate, av_q2d(tb) * 0.5 + 1e-9);
        double residual = t - end;
        /* Some camera/muxer tracks stamp AAC on a coarser cadence than
         * their declared time base (e.g. 16/24/24 ms for 1024 samples).
         * A small OVERLAP proves timestamps cannot represent contiguous
         * decoded blocks. Recover with a bounded sample-clock interpretation,
         * retain the original first PTS, and expose the observed uncertainty.
         * Positive gaps stay explicit until such an overlap has established
         * jitter, and gaps beyond half a decoded block remain explicit. */
        if (residual < -tolerance && fabs(residual) <= (double)count / (2 * a->rate)) {
            if (!a->timing_inferred)
                LOG("AUDIO", "WARN: audio PTS overlap by %.3f ms; recovering with native sample clock, timing is inferred", -residual * 1000);
            a->timing_inferred = true;
            a->jitter_window = fmax(a->jitter_window, (double)count / (2 * a->rate));
        }
        if (a->timing_inferred) tolerance = fmax(tolerance, a->jitter_window);
        gap = fabs(residual) > tolerance;
        if (!gap) {
            double uncertainty = fabs(residual);
            if (uncertainty > a->timestamp_uncertainty) {
                a->timestamp_uncertainty = uncertainty;
                for (int i = 0; i < a->n_markers; i++)
                    if ((a->markers[i].kind == AUDIO_MARK_SAMPLE || a->markers[i].kind == AUDIO_MARK_TRANSIENT) &&
                        a->markers[i].timestamp_uncertainty < uncertainty) {
                        a->markers[i].uncertainty += uncertainty - a->markers[i].timestamp_uncertainty;
                        a->markers[i].timestamp_uncertainty = uncertainty;
                    }
            }
            t = end;
        } else if (residual < -tolerance) {
            ok = false; failure = "audio PTS moved backwards beyond the recoverable jitter window";
        } else {
            a->gap_duration += residual;
            LOG("AUDIO", "INFO: audio timestamp gap at %.6f s: %.3f ms (scheduled silence; waveform remains absent)",
                 end, residual * 1000);
        }
    }
    if (ok) ok = reserve((void **)&a->blocks, &a->cap_blocks, a->n_blocks + 1,
                         sizeof(*a->blocks), AUDIO_INDEX_LIMIT);
    int64_t first = a->samples;
    if (ok) {
        a->blocks[a->n_blocks++] = (AudioBlock){
            .time = t, .end = t + (double)count / a->rate,
            .sample = first, .pts = f->best_effort_timestamp, .tb = tb,
            .offset = a->bytes, .count = count,
        };
        for (int i = 0; i < count && ok; i += AUDIO_PEAK_SAMPLES) {
            int end = i + AUDIO_PEAK_SAMPLES < count ? i + AUDIO_PEAK_SAMPLES : count;
            AudioPeak p = { .time = t + (double)i / a->rate,
                            .end = t + (double)end / a->rate };
            for (int c = 0; c < a->channels; c++) {
                p.low[c] = p.high[c] = pcm[i * a->channels + c];
                if (!isfinite(p.low[c])) { ok = false; failure = "audio contains non-finite samples"; break; }
                for (int j = i + 1; j < end; j++) {
                    float v = pcm[j * a->channels + c];
                    if (!isfinite(v)) { ok = false; failure = "audio contains non-finite samples"; break; }
                    p.low[c] = fminf(p.low[c], v);
                    p.high[c] = fmaxf(p.high[c], v);
                }
            }
            if (ok) ok = peak_add(a, p, 0);
        }
        a->bytes += bytes; a->samples += count;
        a->raw_end = f->best_effort_timestamp * av_q2d(tb) -
                     a->inputs[a->input].origin + (double)count / a->rate;
        a->revision++;
        SDL_BroadcastCondition(a->changed);
    }
    SDL_UnlockMutex(a->mutex);
    if (ok && detector) {
        if (gap) transient_reset(detector, first);
        transient_feed(detector, pcm, count, detected, a);
    }
    av_free(pcm);
    if (!ok) fail(a, failure);
    return ok;
}

static int decode_audio(void *opaque)
{
    Audio *a = opaque;
    AVPacket *p = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    TransientDetector *d = a->detect ? transient_create(a->rate, a->channels) : NULL;
    bool ok = p && f && (!a->detect || d), eof = false;
    while (ok && !cancelled(a)) {
        int r = avcodec_receive_frame(a->cc, f);
        if (r == 0) { ok = store_frame(a, f, d); av_frame_unref(f); continue; }
        if (r == AVERROR_EOF) break;
        if (r != AVERROR(EAGAIN) || eof) { ok = false; break; }
        for (;;) {
            r = av_read_frame(a->fmt, p);
            if (r < 0 || p->stream_index == a->stream || cancelled(a)) break;
            av_packet_unref(p);
        }
        if (cancelled(a)) break;
        if (r == AVERROR_EOF) { eof = true; r = avcodec_send_packet(a->cc, NULL); }
        else if (r >= 0) {
            size_t size = 0;
            const uint8_t *skip = av_packet_get_side_data(p, AV_PKT_DATA_SKIP_SAMPLES, &size);
            if (skip && size >= 10) {
                SDL_LockMutex(a->mutex);
                a->priming_samples += AV_RL32(skip);
                a->padding_samples += AV_RL32(skip + 4);
                SDL_UnlockMutex(a->mutex);
            }
            r = avcodec_send_packet(a->cc, p);
        }
        if (r < 0) ok = false;
        av_packet_unref(p);
    }
    av_frame_free(&f); av_packet_free(&p); transient_destroy(d);
    if (cancelled(a)) return 0;
    if (!ok && !cancelled(a)) {
        SDL_LockMutex(a->mutex); bool already_failed = a->failed; SDL_UnlockMutex(a->mutex);
        if (!already_failed) fail(a, "audio decode or read failed");
    }
    SDL_LockMutex(a->mutex);
    a->finished = true;
    if (a->n_blocks) {
        a->duration = a->blocks[a->n_blocks - 1].end;
        a->presentation_span = a->duration - a->blocks[0].time;
        double sample_duration = (double)a->samples / a->rate;
        LOG("AUDIO", "length audit: %lld decoded sample frames / %d Hz = %.6f s PCM; %.6f s presentation span, %.6f s gaps",
             (long long)a->samples, a->rate, sample_duration, a->presentation_span, a->gap_duration);
        if (a->priming_samples || a->padding_samples)
            LOG("AUDIO", "codec skip metadata: %lld priming, %lld trailing samples (handled by FFmpeg; not trimmed twice)",
                 (long long)a->priming_samples, (long long)a->padding_samples);
        if (!a->failed && a->declared_duration > 0) {
            a->duration_delta = a->presentation_span - a->declared_duration;
            double tolerance = fmax(a->timestamp_uncertainty,
                av_q2d(a->fmt->streams[a->stream]->time_base) + 1.0 / a->rate);
            a->duration_mismatch = fabs(a->duration_delta) > tolerance;
            LOG("AUDIO", "%s: declared audio duration %.6f s; presentation difference %+.3f ms",
                 a->duration_mismatch ? "WARN" : "INFO", a->declared_duration, a->duration_delta * 1000);
            if (a->duration_mismatch && a->duration_delta > 0 && a->cc->frame_size > 0 &&
                a->duration_delta <= (double)a->cc->frame_size / a->rate + tolerance)
                LOG("AUDIO", "difference fits one codec frame; trailing padding is possible but unconfirmed, preserving decoded samples");
        } else if (!a->failed)
            LOG("AUDIO", "declared audio duration unavailable; sample count and presentation span reported independently");
    }
    a->revision++;
    file_audit_locked(a);
    SDL_BroadcastCondition(a->changed);
    SDL_UnlockMutex(a->mutex);
    return 0;
}

static bool index_frame(Audio *a, AudioInput *in, AVFrame *f, AVRational tb)
{
    if (f->best_effort_timestamp == AV_NOPTS_VALUE) return false;
    AudioFrame x = { .pts = f->best_effort_timestamp,
                    .time = f->best_effort_timestamp * av_q2d(tb) - in->origin };
    x.end = x.time + f->duration * av_q2d(tb);
    SDL_LockMutex(a->mutex);
    bool ok = (!in->n_frames || x.time > in->frames[in->n_frames - 1].time) &&
        reserve((void **)&in->frames, &in->cap_frames, in->n_frames + 1,
                sizeof(*in->frames), AUDIO_INDEX_LIMIT);
    if (ok) {
        if (in->n_frames) in->frames[in->n_frames - 1].end = x.time;
        in->frames[in->n_frames++] = x;
        for (int i = 0; i < a->n_markers; i++) {
            AudioMarker *m = &a->markers[i];
            if (m->kind == AUDIO_MARK_FRAME && m->input == (int)(in - a->inputs) && m->pts == x.pts)
                m->frame = in->n_frames - 1;
        }
        a->revision++;
    }
    SDL_UnlockMutex(a->mutex);
    return ok;
}

static int index_video(void *opaque)
{
    Audio *a = opaque;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for (int i = 0; i < a->n_inputs && !cancelled(a); i++) {
        AudioInput *in = &a->inputs[i];
        AVFormatContext *fmt = avformat_alloc_context();
        AVCodecContext *cc = NULL;
        AVPacket *p = av_packet_alloc();
        AVFrame *f = av_frame_alloc();
        bool ok = fmt && p && f, eof = false;
        int stream = -1;
        if (fmt) fmt->interrupt_callback = (AVIOInterruptCB){ cancelled, a };
        if (ok) ok = avformat_open_input(&fmt, in->path, NULL, NULL) >= 0 &&
                     avformat_find_stream_info(fmt, NULL) >= 0;
        if (ok) {
            const AVCodec *codec = NULL;
            stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
            cc = stream >= 0 ? avcodec_alloc_context3(codec) : NULL;
            ok = cc && avcodec_parameters_to_context(cc, fmt->streams[stream]->codecpar) >= 0;
            if (ok) {
                cc->thread_count = 1; /* background index must not consume all cores */
                ok = avcodec_open2(cc, codec, NULL) >= 0;
            }
        }
        while (ok && !cancelled(a)) {
            int r = avcodec_receive_frame(cc, f);
            if (r == 0) { ok = index_frame(a, in, f, fmt->streams[stream]->time_base); av_frame_unref(f); continue; }
            if (r == AVERROR_EOF) break;
            if (r != AVERROR(EAGAIN) || eof) { ok = false; break; }
            do {
                av_packet_unref(p);
                r = av_read_frame(fmt, p);
            } while (r >= 0 && p->stream_index != stream && !cancelled(a));
            if (cancelled(a)) break;
            if (r == AVERROR_EOF) { eof = true; r = avcodec_send_packet(cc, NULL); }
            else if (r >= 0) r = avcodec_send_packet(cc, p);
            if (r < 0) ok = false;
            av_packet_unref(p);
        }
        SDL_LockMutex(a->mutex);
        in->stream = stream; in->complete = ok && !cancelled(a); in->failed = !ok;
        a->revision++;
        file_audit_locked(a);
        SDL_UnlockMutex(a->mutex);
        if (!ok && !cancelled(a)) LOG("AUDIO", "frame indexing failed for input %d; exact grid is partial", i);
        av_frame_free(&f); av_packet_free(&p); avcodec_free_context(&cc); avformat_close_input(&fmt);
    }
    return 0;
}

/* Called with mutex held. Cursor is the end of PCM queued at a fixed input
 * rate. SDL input consumption plus a device-block estimate drives video. */
static double clock_locked(Audio *a)
{
    if (!a->sound) return NAN;
    if (a->paused || a->buffering || !a->started) return a->last_clock;
    if (a->fallback_wall > 0)
        return a->fallback_time + wall() - a->fallback_wall;
    int queued = SDL_GetAudioStreamQueued(a->output);
    if (queued < 0) return NAN;
    double t = a->cursor - (double)queued / (a->channels * sizeof(float) * a->rate) - a->latency;
    return fmax(a->last_clock, t);
}

static int output_audio(void *opaque)
{
    Audio *a = opaque;
    float pcm[2048 * AUDIO_MAX_CHANNELS];
    SDL_LockMutex(a->mutex);
    while (!cancelled(a)) {
        if (a->paused || a->failed || a->output_failed) {
            SDL_WaitConditionTimeout(a->changed, a->mutex, 20); continue;
        }
        int queued = SDL_GetAudioStreamQueued(a->output);
        if (queued < 0) { a->output_failed = true; continue; }
        double buffered = (double)queued / (a->rate * a->channels * sizeof(float));
        a->last_clock = clock_locked(a);
        if (a->started && buffered < 0.001 && !a->finished &&
            block_at(a, a->cursor) == a->n_blocks) {
            SDL_PauseAudioStreamDevice(a->output);
            a->buffering = true; a->started = false;
            a->revision++;
        }
        if (buffered >= 0.20) {
            if (!a->started) {
                a->started = SDL_ResumeAudioStreamDevice(a->output);
                a->buffering = !a->started;
            }
            SDL_WaitConditionTimeout(a->changed, a->mutex, 5); continue;
        }
        int idx = block_at(a, a->cursor), count = 0;
        bool silence = false;
        AudioBlock b = {0};
        if (idx < a->n_blocks) {
            b = a->blocks[idx];
            if (b.time > a->cursor + 0.5 / a->rate) {
                count = (int)fmin(2048, ceil((b.time - a->cursor) * a->rate));
                silence = true;
            } else {
                int skip = (int)llround((a->cursor - b.time) * a->rate);
                if (skip < 0) skip = 0;
                if (skip < b.count) count = b.count - skip;
                if (count > 2048) count = 2048;
                b.offset += (int64_t)skip * a->channels * sizeof(float);
            }
        } else if (a->finished && !a->failed) {
            if (queued > 0) {
                SDL_FlushAudioStream(a->output);
                if (!a->started) {
                    a->started = SDL_ResumeAudioStreamDevice(a->output);
                    a->buffering = false;
                }
            } else if (!a->fallback_wall) {
                /* Preserve clock continuity after the selected audio ends. */
                a->fallback_time = fmax(a->last_clock, a->cursor);
                a->fallback_wall = wall();
                a->started = true; a->buffering = false;
            }
            SDL_WaitConditionTimeout(a->changed, a->mutex, 5); continue;
        }
        if (!count) { a->buffering = !a->started; SDL_WaitConditionTimeout(a->changed, a->mutex, 10); continue; }
        uint64_t generation = a->generation;
        SDL_UnlockMutex(a->mutex);
        bool ok = true;
        if (silence) memset(pcm, 0, count * a->channels * sizeof(float));
        else ok = pread(a->fd, pcm, count * a->channels * sizeof(float), b.offset) ==
                      (ssize_t)(count * a->channels * sizeof(float));
        SDL_LockMutex(a->mutex);
        if (generation != a->generation || a->paused) continue;
        if (!ok || !SDL_PutAudioStreamData(a->output, pcm, count * a->channels * sizeof(float))) {
            a->output_failed = true;
            snprintf(a->error, sizeof(a->error), "audio output/cache read failed: %s", SDL_GetError());
            LOG("AUDIO", "%s", a->error);
            continue;
        }
        a->cursor += (double)count / a->rate;
        if (a->finished && idx == a->n_blocks - 1 && !a->started) {
            SDL_FlushAudioStream(a->output);
            a->started = SDL_ResumeAudioStreamDevice(a->output);
            a->buffering = false;
        }
    }
    SDL_UnlockMutex(a->mutex);
    return 0;
}

Audio *audio_open(const char *const *paths, int n, int input, int track,
                  bool sound, bool detect, const char *markers_path)
{
    if (n < 1 || n > 2 || input < 0 || input >= n || track < 0) return NULL;
    Audio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->mutex = SDL_CreateMutex(); a->changed = SDL_CreateCondition();
    a->n_inputs = n; a->input = input; a->stream = -1;
    a->sound = sound; a->detect = detect; a->paused = true;
    a->markers_path = markers_path ? strdup(markers_path) : NULL;
    if (!a->mutex || !a->changed || (markers_path && !a->markers_path)) goto error;
    for (int i = 0; i < n; i++) {
        a->inputs[i].path = realpath(paths[i], NULL);
        if (!a->inputs[i].path || stat(paths[i], &a->inputs[i].identity) ||
            !S_ISREG(a->inputs[i].identity.st_mode)) {
            LOG("AUDIO", "audio inspection currently requires local regular files"); goto error;
        }
        AVFormatContext *probe = NULL;
        if (avformat_open_input(&probe, paths[i], NULL, NULL) < 0 ||
            avformat_find_stream_info(probe, NULL) < 0) {
            avformat_close_input(&probe); goto error;
        }
        a->inputs[i].origin = media_time_origin(probe);
        a->inputs[i].stream = av_find_best_stream(probe, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        AudioInput *in = &a->inputs[i];
        in->declared_file_duration = probe->duration > 0 && probe->duration != AV_NOPTS_VALUE ?
                                    (double)probe->duration / AV_TIME_BASE : -1;
        int audio_tracks = 0, video_tracks = 0;
        double declared_end = -1;
        for (unsigned k = 0; k < probe->nb_streams; k++) {
            AVStream *s = probe->streams[k];
            if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) video_tracks++;
            else if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio_tracks++;
            else in->other_tracks = true;
            if (s->duration > 0 && s->duration != AV_NOPTS_VALUE && s->start_time != AV_NOPTS_VALUE)
                declared_end = fmax(declared_end, (s->start_time + s->duration) * av_q2d(s->time_base) - in->origin);
        }
        in->other_tracks |= audio_tracks > 1 || video_tracks > 1;
        if (in->declared_file_duration > 0) {
            if (fabs(in->origin) < 1e-9) {
                in->declared_file_end = in->declared_file_duration; in->file_end_known = true;
            } else if (declared_end >= 0) {
                /* MOV/MKV duration may include a nonzero starting offset;
                 * MPEG-TS duration usually excludes it. Resolve using
                 * declared stream bounds; abstain when ambiguous. */
                double relative = fabs(in->declared_file_duration - declared_end);
                double absolute = fabs(in->declared_file_duration - declared_end - in->origin);
                if (fmin(relative, absolute) < .002) {
                    in->declared_file_end = in->declared_file_duration - (absolute < relative ? in->origin : 0);
                    in->file_end_known = true;
                }
            }
        }
        avformat_close_input(&probe);
    }
    a->fmt = avformat_alloc_context();
    if (!a->fmt) goto error;
    a->fmt->interrupt_callback = (AVIOInterruptCB){ cancelled, a };
    if (avformat_open_input(&a->fmt, paths[input], NULL, NULL) < 0 ||
        avformat_find_stream_info(a->fmt, NULL) < 0) goto error;
    int ordinal = 0;
    for (unsigned i = 0; i < a->fmt->nb_streams; i++)
        if (a->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ordinal++ == track)
            { a->stream = (int)i; break; }
    if (a->stream < 0) { LOG("AUDIO", "input %d has no audio track a:%d", input, track); goto error; }
    AVStream *st = a->fmt->streams[a->stream];
    a->declared_duration = st->duration > 0 && st->duration != AV_NOPTS_VALUE ?
                          st->duration * av_q2d(st->time_base) : -1;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    a->cc = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!a->cc || avcodec_parameters_to_context(a->cc, st->codecpar) < 0 ||
        avcodec_open2(a->cc, codec, NULL) < 0) goto error;
    a->rate = a->cc->sample_rate; a->channels = a->cc->ch_layout.nb_channels;
    if (a->rate < 8000 || a->rate > 384000 || a->channels < 1 || a->channels > AUDIO_MAX_CHANNELS) {
        LOG("AUDIO", "supported audio: 8-384 kHz, 1-8 channels"); goto error;
    }
    if (swr_alloc_set_opts2(&a->swr, &a->cc->ch_layout, AV_SAMPLE_FMT_FLT, a->rate,
                           &a->cc->ch_layout, a->cc->sample_fmt, a->rate, 0, NULL) < 0 ||
        swr_init(a->swr) < 0) goto error;
    for (int c = 0; c < a->channels; c++) {
        enum AVChannel ch = av_channel_layout_channel_from_index(&a->cc->ch_layout, c);
        if (ch == AV_CHAN_NONE || ch == AV_CHAN_UNKNOWN)
            snprintf(a->channel_names[c], sizeof(a->channel_names[c]), "CH%d", c + 1);
        else av_channel_name(a->channel_names[c], sizeof(a->channel_names[c]), ch);
    }
    a->duration = st->duration != AV_NOPTS_VALUE ?
        st->duration * av_q2d(st->time_base) : a->fmt->duration / (double)AV_TIME_BASE;
    a->cache = tmpfile();
    if (!a->cache) { LOG("AUDIO", "cannot create temporary PCM file"); goto error; }
    a->fd = fileno(a->cache);
    if (markers_path && !audio_markers_load(a)) goto error;
    if (sound) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) goto error;
        SDL_AudioSpec spec = { .format = SDL_AUDIO_F32, .channels = a->channels, .freq = a->rate };
        a->output = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!a->output) { LOG("AUDIO", "open output: %s", SDL_GetError()); goto error; }
        SDL_AudioSpec device;
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->output), &device, &frames))
            a->latency = a->device_block = (double)frames / device.freq;
    }
    a->decoder_thread = SDL_CreateThread(decode_audio, "audio-analysis", a);
    a->index_thread = SDL_CreateThread(index_video, "audio-frame-index", a);
    if (sound) a->output_thread = SDL_CreateThread(output_audio, "audio-output", a);
    if (!a->decoder_thread || !a->index_thread || (sound && !a->output_thread)) goto error;
    LOG("AUDIO", "input %d a:%d: %d Hz, %d channels; %s%s", input, track,
        a->rate, a->channels, sound ? "sound enabled" : "silent inspection",
        detect ? ", conservative transient detection" : "");
    return a;
error:
    LOG("AUDIO", "could not initialize audio inspection");
    audio_close(a); return NULL;
}

void audio_close(Audio *a)
{
    if (!a) return;
    SDL_SetAtomicInt(&a->stop, 1);
    if (a->mutex) SDL_LockMutex(a->mutex);
    if (a->changed) SDL_BroadcastCondition(a->changed);
    if (a->output) SDL_PauseAudioStreamDevice(a->output);
    if (a->mutex) SDL_UnlockMutex(a->mutex);
    if (a->decoder_thread) SDL_WaitThread(a->decoder_thread, NULL);
    if (a->index_thread) SDL_WaitThread(a->index_thread, NULL);
    if (a->output_thread) SDL_WaitThread(a->output_thread, NULL);
    if (a->output) SDL_DestroyAudioStream(a->output);
    swr_free(&a->swr); avcodec_free_context(&a->cc); avformat_close_input(&a->fmt);
    if (a->cache) fclose(a->cache);
    for (int i = 0; i < a->n_inputs; i++) { free(a->inputs[i].path); free(a->inputs[i].frames); }
    for (int i = 0; i < AUDIO_PEAK_LEVELS; i++) free(a->peaks[i].p);
    free(a->blocks); free(a->markers_path);
    if (a->changed) SDL_DestroyCondition(a->changed);
    if (a->mutex) SDL_DestroyMutex(a->mutex);
    free(a);
}

void audio_info(Audio *a, AudioInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (!a) return;
    SDL_LockMutex(a->mutex);
    *info = (AudioInfo){ .rate = a->rate, .channels = a->channels,
        .ready = a->finished, .failed = a->failed || a->output_failed,
        .buffering = a->buffering, .playing = a->sound && !a->paused,
        .duration = a->duration, .latency = a->latency,
        .timing_inferred = a->timing_inferred, .timestamp_uncertainty = a->timestamp_uncertainty,
        .decoded_samples = a->samples, .priming_samples = a->priming_samples,
        .padding_samples = a->padding_samples, .sample_duration = (double)a->samples / a->rate,
        .presentation_span = a->presentation_span, .gap_duration = a->gap_duration,
        .declared_duration = a->declared_duration, .duration_delta = a->duration_delta,
        .duration_mismatch = a->duration_mismatch,
        .file_audit_complete = a->file_audit_complete, .file_duration_mismatch = a->file_duration_mismatch,
        .declared_file_duration = a->inputs[a->input].declared_file_duration,
        .file_observed_end = a->file_observed_end, .file_duration_delta = a->file_duration_delta,
        .revision = a->revision, .markers = a->n_markers,
        .decoded_end = a->n_blocks ? a->blocks[a->n_blocks - 1].end : 0 };
    for (int i = 0; i < a->n_markers; i++)
        if (a->markers[i].kind == AUDIO_MARK_TRANSIENT && !a->markers[i].dismissed) info->transients++;
    snprintf(info->error, sizeof(info->error), "%s", a->error);
    memcpy(info->channel_names, a->channel_names, sizeof(info->channel_names));
    SDL_UnlockMutex(a->mutex);
}

void audio_transport(Audio *a, double time, bool paused, bool seek)
{
    if (!a || !a->sound || !isfinite(time)) return;
    SDL_LockMutex(a->mutex);
    if (seek || paused != a->paused) {
        SDL_PauseAudioStreamDevice(a->output);
        if (!seek) time = clock_locked(a);
        SDL_ClearAudioStream(a->output);
        a->generation++;
        a->target = a->cursor = a->last_clock = fmax(0, time);
        a->fallback_wall = 0;
        a->started = false;
        a->buffering = !paused;
        a->paused = paused;
        a->revision++;
    }
    SDL_BroadcastCondition(a->changed);
    SDL_UnlockMutex(a->mutex);
}

double audio_clock(Audio *a)
{
    if (!a) return NAN;
    SDL_LockMutex(a->mutex);
    double t = a->failed || a->output_failed ? NAN : clock_locked(a);
    if (isfinite(t)) a->last_clock = t;
    SDL_UnlockMutex(a->mutex);
    return t;
}

void audio_mute(Audio *a, bool muted)
{
    if (!a || !a->output) return;
    SDL_LockMutex(a->mutex);
    if (SDL_SetAudioStreamGain(a->output, muted ? 0 : 1)) a->muted = muted;
    SDL_UnlockMutex(a->mutex);
}

bool audio_marker_add(Audio *a, const AudioMarker *m)
{
    if (!a || !m || !isfinite(m->time)) return false;
    SDL_LockMutex(a->mutex);
    bool ok = audio_append_marker(a, m);
    SDL_UnlockMutex(a->mutex);
    return ok;
}

bool audio_marker_sample(Audio *a, double time, int channel)
{
    if (!a || !isfinite(time) || channel < 0 || channel >= a->channels) return false;
    SDL_LockMutex(a->mutex);
    int idx = block_at(a, time);
    bool ok = false;
    if (idx < a->n_blocks && time >= a->blocks[idx].time - 0.5 / a->rate) {
        AudioBlock *b = &a->blocks[idx];
        int offset = (int)llround((time - b->time) * a->rate);
        if (offset < 0) offset = 0;
        if (offset >= b->count) offset = b->count - 1;
        AudioMarker m = { .kind = AUDIO_MARK_SAMPLE, .input = a->input, .stream = a->stream,
            .channel = channel, .time = b->time + (double)offset / a->rate,
            .sample = b->sample + offset, .pts = b->pts, .tb = b->tb, .frame = -1,
            .uncertainty = a->timestamp_uncertainty,
            .timestamp_uncertainty = a->timestamp_uncertainty, .reviewed = true };
        snprintf(m.label, sizeof(m.label), "AUDIO CH%d", channel + 1);
        ok = audio_append_marker(a, &m);
    }
    SDL_UnlockMutex(a->mutex);
    return ok;
}

bool audio_marker_time(Audio *a, double time)
{
    AudioMarker m = { .kind = AUDIO_MARK_TIME, .time = time, .input = a ? a->input : 0,
        .stream = -1, .channel = -1, .frame = -1, .sample = -1, .reviewed = true, .tb = {1, 1} };
    snprintf(m.label, sizeof(m.label), "TIME");
    return audio_marker_add(a, &m);
}

int64_t audio_frame_ordinal(Audio *a, int input, int64_t pts)
{
    if (!a || input < 0 || input >= a->n_inputs) return -1;
    SDL_LockMutex(a->mutex);
    AudioInput *in = &a->inputs[input];
    int lo = 0, hi = in->n_frames;
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (in->frames[m].pts < pts) lo = m + 1; else hi = m; }
    int64_t result = lo < in->n_frames && in->frames[lo].pts == pts ? lo : -1;
    SDL_UnlockMutex(a->mutex);
    return result;
}

bool audio_marker_frame(Audio *a, int input, int stream, int64_t pts,
                        AVRational tb, double time, double end)
{
    if (!a || pts == AV_NOPTS_VALUE || input < 0 || input >= a->n_inputs) return false;
    AudioMarker m = { .kind = AUDIO_MARK_FRAME, .input = input, .stream = stream,
        .pts = pts, .tb = tb, .time = time, .end = end, .sample = -1, .channel = -1,
        .frame = audio_frame_ordinal(a, input, pts), .reviewed = true };
    snprintf(m.label, sizeof(m.label), "VIDEO %d", input);
    return audio_marker_add(a, &m);
}

int audio_markers(Audio *a, AudioMarker *out, int cap)
{
    if (!a || !out || cap <= 0) return 0;
    SDL_LockMutex(a->mutex);
    int n = a->n_markers < cap ? a->n_markers : cap;
    memcpy(out, a->markers, n * sizeof(*out));
    SDL_UnlockMutex(a->mutex);
    return n;
}

bool audio_marker_review(Audio *a, int index, bool dismiss)
{
    if (!a) return false;
    SDL_LockMutex(a->mutex);
    bool ok = index >= 0 && index < a->n_markers;
    if (ok) { a->markers[index].reviewed = !dismiss; a->markers[index].dismissed = dismiss; a->revision++; }
    SDL_UnlockMutex(a->mutex);
    return ok;
}

bool audio_marker_label(Audio *a, int index, const char *label)
{
    if (!a || !label) return false;
    SDL_LockMutex(a->mutex);
    bool ok = index >= 0 && index < a->n_markers;
    if (ok) {
        snprintf(a->markers[index].label, sizeof(a->markers[index].label), "%s", label);
        a->revision++;
    }
    SDL_UnlockMutex(a->mutex);
    return ok;
}

bool audio_marker_move(Audio *a, int index, double time, int channel)
{
    if (!a || !isfinite(time) || channel < 0 || channel >= a->channels) return false;
    SDL_LockMutex(a->mutex);
    int bidx = block_at(a, time);
    bool ok = index >= 0 && index < a->n_markers && bidx < a->n_blocks &&
              time >= a->blocks[bidx].time;
    if (ok) {
        AudioBlock *b = &a->blocks[bidx];
        int offset = (int)llround((time - b->time) * a->rate);
        if (offset >= b->count) offset = b->count - 1;
        AudioMarker *m = &a->markers[index];
        if (m->kind == AUDIO_MARK_FRAME) ok = false;
        else {
            if (m->kind != AUDIO_MARK_TRANSIENT) m->kind = AUDIO_MARK_SAMPLE;
            m->input = a->input; m->stream = a->stream;
            m->time = b->time + (double)offset / a->rate;
            m->pts = b->pts; m->tb = b->tb; m->sample = b->sample + offset;
            m->channel = channel; m->reviewed = true; m->dismissed = false;
            m->uncertainty = a->timestamp_uncertainty; a->revision++;
            m->timestamp_uncertainty = a->timestamp_uncertainty;
        }
    }
    SDL_UnlockMutex(a->mutex);
    return ok;
}

bool audio_markers_save(Audio *a)
{ return a && a->markers_path && audio_markers_write(a); }

bool audio_transient_near(Audio *a, double time, int direction, AudioMarker *out, int *index)
{
    if (!a || !isfinite(time)) return false;
    SDL_LockMutex(a->mutex);
    double distance = INFINITY;
    int best = -1;
    for (int i = 0; i < a->n_markers; i++) {
        AudioMarker *m = &a->markers[i];
        if (m->kind != AUDIO_MARK_TRANSIENT || m->dismissed) continue;
        double d = m->time - time;
        if ((direction > 0 && d <= 0.005) || (direction < 0 && d >= -0.005)) continue;
        if (fabs(d) < distance) { distance = fabs(d); best = i; }
    }
    if (best >= 0) { if (out) *out = a->markers[best]; if (index) *index = best; }
    SDL_UnlockMutex(a->mutex);
    return best >= 0;
}

/* Queries are bounded to the viewport; lock protects growing index vectors.
 * Source sample zoom uses pread(), independent of the writer's file cursor. */
void audio_envelope(Audio *a, double start, double span, int width, int channel,
                     float *low, float *high)
{
    if (!a || width <= 0 || span <= 0 || channel < 0 || channel >= a->channels) return;
    for (int x = 0; x < width; x++) low[x] = high[x] = NAN;
    SDL_LockMutex(a->mutex);
    double pixel = span / width;
    if (pixel * a->rate < AUDIO_PEAK_SAMPLES) {
        int idx = block_at(a, start);
        float pcm[2048 * AUDIO_MAX_CHANNELS];
        for (; idx < a->n_blocks && a->blocks[idx].time < start + span; idx++) {
            AudioBlock b = a->blocks[idx];
            int begin = (int)fmax(0, floor((start - b.time) * a->rate));
            int end = (int)fmin(b.count, ceil((start + span - b.time) * a->rate));
            for (int j = begin; j < end; j += 2048) {
                int n = end - j < 2048 ? end - j : 2048;
                if (pread(a->fd, pcm, n * a->channels * sizeof(float),
                          b.offset + (int64_t)j * a->channels * sizeof(float)) !=
                    (ssize_t)(n * a->channels * sizeof(float))) break;
                for (int k = 0; k < n; k++) {
                    int x = (int)floor((b.time + (double)(j + k) / a->rate - start) / pixel);
                    if (x < 0 || x >= width) continue;
                    float v = pcm[k * a->channels + channel];
                    low[x] = isnan(low[x]) ? v : fminf(low[x], v);
                    high[x] = isnan(high[x]) ? v : fmaxf(high[x], v);
                }
            }
        }
    } else {
        /* Descend when a parent spans a viewport boundary or a pixel.
         * Base bins handle tails and gaps absent from higher levels. */
        int level = 0;
        double interval = (double)AUDIO_PEAK_SAMPLES / a->rate;
        while (level + 1 < AUDIO_PEAK_LEVELS && interval * 4 <= pixel &&
               a->peaks[level + 1].n) { level++; interval *= 4; }
        AudioPeaks *p = &a->peaks[level];
        int lo = 0, hi = p->n;
        while (lo < hi) { int m = lo + (hi - lo) / 2; if (p->p[m].end <= start) lo = m + 1; else hi = m; }
        for (int i = lo; i < p->n && p->p[i].time < start + span; i++) {
            int x0 = (int)floor((p->p[i].time - start) / pixel);
            int x1 = (int)floor((p->p[i].end - start) / pixel);
            if (x0 < 0) x0 = 0; if (x1 >= width) x1 = width - 1;
            for (int x = x0; x <= x1; x++) {
                low[x] = isnan(low[x]) ? p->p[i].low[channel] : fminf(low[x], p->p[i].low[channel]);
                high[x] = isnan(high[x]) ? p->p[i].high[channel] : fmaxf(high[x], p->p[i].high[channel]);
            }
        }
        /* Base bins not covered by a parent include incomplete tails and
         * gaps. Merge ALL their peaks, even into an occupied pixel. */
        if (level) {
            AudioPeaks *parents = p;
            int parent = 0;
            p = &a->peaks[0]; lo = 0; hi = p->n;
            while (lo < hi) { int m = lo + (hi - lo) / 2; if (p->p[m].end <= start) lo = m + 1; else hi = m; }
            for (int i = lo; i < p->n && p->p[i].time < start + span; i++) {
                while (parent < parents->n && parents->p[parent].last < i) parent++;
                if (parent < parents->n && parents->p[parent].first <= i &&
                    parents->p[parent].last >= i) continue;
                int x = (int)floor((p->p[i].time - start) / pixel);
                if (x >= 0 && x < width) {
                    low[x] = isnan(low[x]) ? p->p[i].low[channel] : fminf(low[x], p->p[i].low[channel]);
                    high[x] = isnan(high[x]) ? p->p[i].high[channel] : fmaxf(high[x], p->p[i].high[channel]);
                }
            }
        }
    }
    SDL_UnlockMutex(a->mutex);
}

int audio_frame_grid(Audio *a, int input, double start, double end,
                      double *times, int64_t *ordinals, int cap)
{
    if (!a || input < 0 || input >= a->n_inputs || cap <= 0) return 0;
    SDL_LockMutex(a->mutex);
    AudioInput *in = &a->inputs[input];
    int lo = 0, hi = in->n_frames;
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (in->frames[m].time < start) lo = m + 1; else hi = m; }
    int n = 0;
    int last = lo;
    while (last < in->n_frames && in->frames[last].time <= end) last++;
    int stride = (last - lo + cap - 1) / cap;
    if (stride < 1) stride = 1;
    for (int i = lo; i < last && n < cap; i += stride) {
        times[n] = in->frames[i].time; ordinals[n++] = i;
    }
    SDL_UnlockMutex(a->mutex);
    return n;
}

double audio_input_origin(Audio *a, int input)
{ return a && input >= 0 && input < a->n_inputs ? a->inputs[input].origin : 0; }
