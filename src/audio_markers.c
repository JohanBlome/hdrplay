#include "audio_internal.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>

/* Small strict JSON reader for the versioned sidecar. Tokens retain source
 * spelling, so 64-bit PTS/sample positions never pass through a double. */
typedef struct { int start, end, next; char type; } Token;
typedef struct { const char *s; size_t len, pos; Token *t; int n, cap; } Json;
static void ws(Json *j) { while (j->pos < j->len && isspace((unsigned char)j->s[j->pos])) j->pos++; }
static int value(Json *j, int depth)
{
    ws(j);
    if (depth > 16 || j->n == j->cap || j->pos == j->len) return -1;
    int id = j->n++;
    Token *t = &j->t[id];
    t->start = (int)j->pos; t->type = j->s[j->pos++];
    if (t->type == '{' || t->type == '[') {
        char close = t->type == '{' ? '}' : ']';
        ws(j);
        if (j->pos < j->len && j->s[j->pos] == close) j->pos++;
        else for (;;) {
            if (t->type == '{') {
                ws(j);
                if (j->pos == j->len || j->s[j->pos] != '"' || value(j, depth + 1) < 0) return -1;
                ws(j); if (j->pos == j->len || j->s[j->pos++] != ':') return -1;
            }
            if (value(j, depth + 1) < 0) return -1;
            ws(j); if (j->pos == j->len) return -1;
            char c = j->s[j->pos++];
            if (c == close) break;
            if (c != ',') return -1;
        }
    } else if (t->type == '"') {
        bool closed = false;
        while (j->pos < j->len) {
            unsigned char c = j->s[j->pos++];
            if (c == '"') { closed = true; break; }
            if (c < 32) return -1;
            if (c == '\\') {
                if (j->pos == j->len) return -1;
                c = j->s[j->pos++];
                if (c == 'u') {
                    for (int k = 0; k < 4; k++)
                        if (j->pos == j->len || !isxdigit((unsigned char)j->s[j->pos++])) return -1;
                } else if (!strchr("\"\\/bfnrt", c)) return -1;
            }
        }
        if (!closed) return -1;
    } else {
        while (j->pos < j->len && !isspace((unsigned char)j->s[j->pos]) &&
               !strchr(",]}", j->s[j->pos])) j->pos++;
        size_t n = j->pos - t->start;
        if (t->type == 't' || t->type == 'f' || t->type == 'n') {
            const char *v = t->type == 't' ? "true" : t->type == 'f' ? "false" : "null";
            if (n != strlen(v) || strncmp(j->s + t->start, v, n)) return -1;
        } else {
            /* JSON number grammar; reject NaN, inf, leading zeros, etc. */
            const char *p = j->s + t->start, *end = j->s + j->pos;
            if (*p == '-') p++;
            if (p == end || !isdigit((unsigned char)*p)) return -1;
            if (*p == '0') p++; else while (p < end && isdigit((unsigned char)*p)) p++;
            if (p < end && *p == '.') { p++; if (p == end || !isdigit((unsigned char)*p)) return -1; while (p < end && isdigit((unsigned char)*p)) p++; }
            if (p < end && (*p == 'e' || *p == 'E')) { p++; if (p < end && (*p == '+' || *p == '-')) p++; if (p == end || !isdigit((unsigned char)*p)) return -1; while (p < end && isdigit((unsigned char)*p)) p++; }
            if (p != end) return -1;
            t->type = '#';
        }
    }
    t->end = (int)j->pos; t->next = j->n;
    return id;
}
static bool equal(Json *j, int i, const char *s)
{
    if (i < 0 || j->t[i].type != '"') return false;
    Token t = j->t[i];
    return t.end - t.start - 2 == (int)strlen(s) &&
           !strncmp(j->s + t.start + 1, s, strlen(s));
}
static int field(Json *j, int obj, const char *key)
{
    if (obj < 0 || j->t[obj].type != '{') return -1;
    int found = -1;
    for (int i = obj + 1; i < j->t[obj].next;) {
        int v = i + 1;
        if (equal(j, i, key)) { if (found >= 0) return -1; found = v; }
        i = j->t[v].next;
    }
    return found;
}
static bool spelling(Json *j, int i, char *buf, size_t size)
{
    if (i < 0) return false;
    int n = j->t[i].end - j->t[i].start;
    if (n < 1 || (size_t)n >= size) return false;
    memcpy(buf, j->s + j->t[i].start, n); buf[n] = 0;
    return true;
}
static bool integer(Json *j, int i, int64_t *out)
{
    char b[64], *end;
    if (i < 0 || j->t[i].type != '#' || !spelling(j, i, b, sizeof(b))) return false;
    errno = 0; long long v = strtoll(b, &end, 10);
    if (errno || *end) return false;
    *out = v; return true;
}
static bool number(Json *j, int i, double *out)
{
    char b[96], *end;
    if (i < 0 || j->t[i].type != '#' || !spelling(j, i, b, sizeof(b))) return false;
    errno = 0; double v = strtod(b, &end);
    if (errno || *end || !isfinite(v)) return false;
    *out = v; return true;
}
static bool boolean(Json *j, int i, bool *out)
{
    if (i < 0 || (j->t[i].type != 't' && j->t[i].type != 'f')) return false;
    *out = j->t[i].type == 't'; return true;
}
static bool string(Json *j, int i, char *buf, size_t cap)
{
    if (i < 0 || j->t[i].type != '"') return false;
    size_t n = 0;
    for (int p = j->t[i].start + 1; p < j->t[i].end - 1; p++) {
        unsigned char c = j->s[p];
        if (c == '\\') {
            c = j->s[++p];
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t'; else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'u') {
                char hex[5]; memcpy(hex, j->s + p + 1, 4); hex[4] = 0;
                unsigned v = (unsigned)strtoul(hex, NULL, 16); p += 4;
                if (v == 0 || v > 127) return false; /* writer emits literal UTF-8 */
                c = (unsigned char)v;
            }
        }
        if (n + 1 >= cap) return false;
        buf[n++] = c;
    }
    buf[n] = 0; return true;
}
static void quote(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p < 32) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
static int64_t mtime_nsec(const struct stat *st)
{
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000 + st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
#endif
}

bool audio_markers_load(Audio *a)
{
    FILE *f = fopen(a->markers_path, "rb");
    if (!f) {
        if (errno == ENOENT) return true;
        LOG("AUDIO", "cannot read markers: %s", strerror(errno)); return false;
    }
    bool ok = false;
    char *data = NULL;
    Token *tokens = NULL;
    AudioMarker *markers = NULL;
    if (fseek(f, 0, SEEK_END)) goto done;
    long len = ftell(f);
    if (len < 2 || len > 8 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) goto done;
    data = malloc((size_t)len + 1);
    tokens = calloc(150000, sizeof(*tokens));
    markers = calloc(AUDIO_MAX_MARKERS, sizeof(*markers));
    if (!data || !tokens || !markers || fread(data, 1, len, f) != (size_t)len) goto done;
    data[len] = 0;
    Json j = { .s = data, .len = (size_t)len, .t = tokens, .cap = 150000 };
    if (value(&j, 0) != 0) goto done;
    ws(&j); if (j.pos != j.len || tokens[0].type != '{') goto done;
    int64_t v;
    if (!integer(&j, field(&j, 0, "version"), &v) || v != 1) goto done;
    if (!integer(&j, field(&j, 0, "detector_version"), &v) || v != TRANSIENT_VERSION) goto done;
    if (!integer(&j, field(&j, 0, "rate"), &v) || v != a->rate) goto done;
    if (!integer(&j, field(&j, 0, "audio_input"), &v) || v != a->input) goto done;
    if (!integer(&j, field(&j, 0, "audio_stream"), &v) || v != a->stream) goto done;
    int sources = field(&j, 0, "sources");
    if (sources < 0 || tokens[sources].type != '[') goto done;
    int s = sources + 1;
    for (int i = 0; i < a->n_inputs; i++) {
        char path[4096];
        if (s >= tokens[sources].next || !string(&j, field(&j, s, "path"), path, sizeof(path)) ||
            strcmp(path, a->inputs[i].path)) goto done;
        if (!integer(&j, field(&j, s, "size"), &v) || v != a->inputs[i].identity.st_size) goto done;
        if (!integer(&j, field(&j, s, "mtime_ns"), &v) || v != mtime_nsec(&a->inputs[i].identity)) goto done;
        s = tokens[s].next;
    }
    if (s != tokens[sources].next) goto done;
    int array = field(&j, 0, "markers"), count = 0;
    if (array < 0 || tokens[array].type != '[') goto done;
    for (int obj = array + 1; obj < tokens[array].next; obj = tokens[obj].next) {
        if (count == AUDIO_MAX_MARKERS || tokens[obj].type != '{') goto done;
        AudioMarker *m = &markers[count++];
#define INT_FIELD(key, dst, min, max) do { \
    if (!integer(&j, field(&j, obj, key), &v) || v < (min) || v > (max)) goto done; \
    (dst) = v; \
} while (0)
        INT_FIELD("kind", m->kind, AUDIO_MARK_SAMPLE, AUDIO_MARK_TRANSIENT);
        INT_FIELD("input", m->input, 0, a->n_inputs - 1);
        INT_FIELD("stream", m->stream, -1, 65535);
        INT_FIELD("channel", m->channel, -1, a->channels - 1);
        INT_FIELD("pts", m->pts, INT64_MIN + 1, INT64_MAX);
        INT_FIELD("sample", m->sample, -1, INT64_MAX);
        INT_FIELD("detected_sample", m->detected_sample, -1, INT64_MAX);
        INT_FIELD("detected_channel", m->detected_channel, -1, a->channels - 1);
        INT_FIELD("frame", m->frame, -1, INT64_MAX);
        INT_FIELD("tb_num", m->tb.num, 1, INT32_MAX);
        INT_FIELD("tb_den", m->tb.den, 1, INT32_MAX);
#undef INT_FIELD
        double peak, contrast;
        if (!number(&j, field(&j, obj, "time"), &m->time) ||
            !number(&j, field(&j, obj, "end"), &m->end) ||
            !number(&j, field(&j, obj, "uncertainty"), &m->uncertainty) || m->uncertainty < 0 ||
            !number(&j, field(&j, obj, "timestamp_uncertainty"), &m->timestamp_uncertainty) ||
            m->timestamp_uncertainty < 0 || m->timestamp_uncertainty > m->uncertainty ||
            !number(&j, field(&j, obj, "peak"), &peak) || peak < 0 || peak > 1000 ||
            !number(&j, field(&j, obj, "contrast_db"), &contrast) || fabs(contrast) > 1000 ||
            !boolean(&j, field(&j, obj, "reviewed"), &m->reviewed) ||
            !boolean(&j, field(&j, obj, "dismissed"), &m->dismissed) ||
            !string(&j, field(&j, obj, "label"), m->label, sizeof(m->label))) goto done;
        m->peak = (float)peak; m->contrast_db = (float)contrast;
        if ((m->kind == AUDIO_MARK_SAMPLE || m->kind == AUDIO_MARK_TRANSIENT) &&
            (m->input != a->input || m->stream != a->stream || m->channel < 0 || m->sample < 0)) goto done;
        if (m->kind == AUDIO_MARK_FRAME && m->end < m->time) goto done;
        if (m->kind == AUDIO_MARK_TRANSIENT && (m->detected_sample < 0 || m->detected_channel < 0)) goto done;
    }
    memcpy(a->markers, markers, count * sizeof(*markers)); a->n_markers = count;
    ok = true;
done:
    fclose(f); free(data); free(tokens); free(markers);
    if (!ok) LOG("AUDIO", "markers rejected: invalid schema, track mismatch, or source file changed (%s)", a->markers_path);
    return ok;
}

bool audio_markers_write(Audio *a)
{
    /* Snapshot under the mutex; slow filesystem writes never block output. */
    AudioMarker *markers = calloc(AUDIO_MAX_MARKERS, sizeof(*markers));
    if (!markers) return false;
    int count = audio_markers(a, markers, AUDIO_MAX_MARKERS);
    AudioInfo info; audio_info(a, &info);
    size_t size = strlen(a->markers_path) + 16;
    char *temp = malloc(size);
    if (!temp) { free(markers); return false; }
    snprintf(temp, size, "%s.tmp.XXXXXX", a->markers_path);
    int fd = mkstemp(temp);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) { if (fd >= 0) close(fd); free(temp); free(markers); return false; }
    fprintf(f, "{\n  \"version\":1,\"detector_version\":%d,\"rate\":%d,\"audio_input\":%d,\"audio_stream\":%d,\n",
             TRANSIENT_VERSION, a->rate, a->input, a->stream);
    fprintf(f, "  \"timing_inferred\":%s,\"timestamp_uncertainty\":%.17g,\n",
             info.timing_inferred ? "true" : "false", info.timestamp_uncertainty);
    fprintf(f, "  \"length_audit\":{\"complete\":%s,\"decoded_samples\":%lld,\"sample_duration\":%.17g,\"presentation_span\":%.17g,\"gaps\":%.17g,\"declared_duration\":%.17g,\"difference\":%.17g,\"mismatch\":%s,\"priming_samples\":%lld,\"padding_samples\":%lld},\n",
             info.ready && !info.failed ? "true" : "false", (long long)info.decoded_samples,
             info.sample_duration, info.presentation_span, info.gap_duration,
             info.declared_duration, info.duration_delta, info.duration_mismatch ? "true" : "false",
             (long long)info.priming_samples, (long long)info.padding_samples);
    fprintf(f, "  \"file_length_audit\":{\"complete\":%s,\"declared_duration\":%.17g,\"observed_timeline_end\":%.17g,\"difference\":%.17g,\"mismatch\":%s},\n",
             info.file_audit_complete ? "true" : "false", info.declared_file_duration,
             info.file_observed_end, info.file_duration_delta, info.file_duration_mismatch ? "true" : "false");
    fprintf(f, "  \"detector\":{\"baseline_ms\":50,\"attack_ms\":2,\"window_ms\":10,\"contrast_db\":20,\"peak_dbfs\":-18,\"crest_db\":12},\n  \"sources\":[");
    for (int i = 0; i < a->n_inputs; i++) {
        if (i) fputc(',', f);
        fprintf(f, "{\"path\":"); quote(f, a->inputs[i].path);
        fprintf(f, ",\"size\":%lld,\"mtime_ns\":%lld}",
                 (long long)a->inputs[i].identity.st_size, (long long)mtime_nsec(&a->inputs[i].identity));
    }
    fprintf(f, "],\n  \"markers\":[\n");
    for (int i = 0; i < count; i++) {
        AudioMarker *m = &markers[i];
        fprintf(f, "    {\"kind\":%d,\"input\":%d,\"stream\":%d,\"channel\":%d,\"pts\":%lld,\"sample\":%lld,\"frame\":%lld,\"tb_num\":%d,\"tb_den\":%d,",
                 m->kind, m->input, m->stream, m->channel, (long long)m->pts,
                 (long long)m->sample, (long long)m->frame, m->tb.num, m->tb.den);
        fprintf(f, "\"detected_sample\":%lld,\"detected_channel\":%d,",
                 (long long)(m->kind == AUDIO_MARK_TRANSIENT ? m->detected_sample : -1),
                 m->kind == AUDIO_MARK_TRANSIENT ? m->detected_channel : -1);
        fprintf(f, "\"timestamp_uncertainty\":%.17g,", m->timestamp_uncertainty);
        fprintf(f, "\"time\":%.17g,\"end\":%.17g,\"uncertainty\":%.17g,\"peak\":%.9g,\"contrast_db\":%.9g,\"reviewed\":%s,\"dismissed\":%s,\"label\":",
                 m->time, m->end, m->uncertainty, m->peak, m->contrast_db,
                 m->reviewed ? "true" : "false", m->dismissed ? "true" : "false");
        quote(f, m->label); fprintf(f, "}%s\n", i + 1 < count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fd) == 0;
    if (fclose(f)) ok = false;
    if (ok) ok = rename(temp, a->markers_path) == 0;
    if (!ok) unlink(temp);
    else LOG("AUDIO", "saved %d markers to %s", count, a->markers_path);
    free(temp); free(markers);
    return ok;
}
