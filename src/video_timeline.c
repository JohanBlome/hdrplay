#include "video_timeline.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavutil/mathematics.h>

#define TAG(a,b,c,d) ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | (uint32_t)(c)<<8 | (d))
#define MAX_SAMPLES 2000000
#define MAX_TABLE_BYTES (32 * 1024 * 1024)
typedef struct { uint64_t data, end; uint32_t type; } Box;
typedef struct { unsigned char *p; size_t n; } Table;
typedef struct { Table stts, ctts, stss, stsz, elst; uint32_t scale; } Track;
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint64_t u64(const unsigned char *p)
{ return (uint64_t)u32(p)<<32 | u32(p+4); }
static bool read_at(int fd, uint64_t offset, void *out, size_t n)
{ return offset <= INT64_MAX && pread(fd, out, n, (off_t)offset) == (ssize_t)n; }
static bool box_at(int fd, uint64_t pos, uint64_t end, Box *box)
{
    unsigned char h[16];
    if (pos > end || end-pos < 8 || !read_at(fd,pos,h,8)) return false;
    uint64_t size=u32(h); unsigned header=8;
    if (size==1) { if (end-pos<16 || !read_at(fd,pos+8,h+8,8)) return false; size=u64(h+8); header=16; }
    else if (!size) size=end-pos;
    if (size<header || size>end-pos) return false;
    *box=(Box){pos+header,pos+size,u32(h+4)};
    return true;
}
static bool table_read(int fd, Box box, Table *table)
{
    if (table->p || box.end-box.data>MAX_TABLE_BYTES) return false;
    table->n=(size_t)(box.end-box.data);
    table->p=malloc(table->n ? table->n : 1);
    return table->p && read_at(fd,box.data,table->p,table->n);
}
static bool scale_read(int fd, Box box, uint32_t *scale)
{
    unsigned char p[24];
    size_t n=box.end-box.data < sizeof(p) ? (size_t)(box.end-box.data) : sizeof(p);
    if (n<16 || !read_at(fd,box.data,p,n) || p[0]>1) return false;
    size_t at=p[0] ? 20 : 12;
    if (n<at+4) return false;
    *scale=u32(p+at);
    return *scale>0 && *scale<=INT_MAX;
}
static bool track_id_read(int fd, Box trak, int *id)
{
    for (uint64_t pos=trak.data; pos<trak.end;) {
        Box b; if (!box_at(fd,pos,trak.end,&b)) return false;
        if (b.type==TAG('t','k','h','d')) {
            unsigned char p[24]; size_t n=b.end-b.data < sizeof(p) ? (size_t)(b.end-b.data) : sizeof(p);
            if (n<16 || !read_at(fd,b.data,p,n) || p[0]>1) return false;
            size_t at=p[0] ? 20 : 12;
            if (n<at+4 || u32(p+at)>INT_MAX) return false;
            *id=(int)u32(p+at); return true;
        }
        pos=b.end;
    }
    return false;
}
static bool track_read(int fd, Box parent, Track *track, int depth)
{
    if (depth>5) return false;
    for (uint64_t pos=parent.data; pos<parent.end;) {
        Box b; if (!box_at(fd,pos,parent.end,&b)) return false;
        Table *table=NULL;
        switch (b.type) {
        case TAG('m','d','i','a'): case TAG('m','i','n','f'): case TAG('s','t','b','l'): case TAG('e','d','t','s'):
            if (!track_read(fd,b,track,depth+1)) return false; break;
        case TAG('m','d','h','d'): if (!scale_read(fd,b,&track->scale)) return false; break;
        case TAG('s','t','t','s'): table=&track->stts; break;
        case TAG('c','t','t','s'): table=&track->ctts; break;
        case TAG('s','t','s','s'): table=&track->stss; break;
        case TAG('s','t','s','z'): case TAG('s','t','z','2'): table=&track->stsz; break;
        case TAG('e','l','s','t'): table=&track->elst; break;
        }
        if (table && !table_read(fd,b,table)) return false;
        pos=b.end;
    }
    return true;
}
static void track_free(Track *t)
{ free(t->stts.p); free(t->ctts.p); free(t->stss.p); free(t->stsz.p); free(t->elst.p); }
static bool entries(Table t, unsigned stride, uint32_t *count)
{
    if (t.n<8) return false;
    *count=u32(t.p+4);
    return *count<=MAX_SAMPLES && *count<=(t.n-8)/stride;
}
static int sample_compare(const void *a, const void *b)
{
    const VideoSample *x=a,*y=b;
    return (x->pts>y->pts)-(x->pts<y->pts);
}
static bool samples_read(Track *t, uint32_t movie_scale, AVRational tb,
                          VideoSample **out, int *count, const char **reason)
{
    uint32_t runs;
    *reason="invalid MOV/MP4 video timing tables";
    if (!t->scale || !entries(t->stts,8,&runs) || t->stts.p[0]!=0) return false;
    uint64_t total=0;
    for (uint32_t i=0;i<runs;i++) total+=u32(t->stts.p+8+i*8);
    if (!total || total>MAX_SAMPLES) { *reason="empty or oversized MOV/MP4 sample table (fragmented files are unsupported)"; return false; }
    if (t->stsz.n<12 || u32(t->stsz.p+8)!=total) return false;
    VideoSample *samples=calloc((size_t)total,sizeof(*samples));
    if (!samples) return false;
    int n=0; int64_t dts=0;
    for (uint32_t i=0;i<runs;i++) {
        uint32_t length=u32(t->stts.p+8+i*8), delta=u32(t->stts.p+12+i*8);
        if (!delta || (length && delta>(uint64_t)(INT64_MAX-dts)/length)) goto fail;
        for (uint32_t j=0;j<length;j++) {
            samples[n++]=(VideoSample){.pts=dts,.duration=delta,.key=!t->stss.p}; dts+=delta;
        }
    }
    if (t->ctts.p) {
        if (t->ctts.p[0]>1 || !entries(t->ctts,8,&runs)) goto fail;
        n=0;
        for (uint32_t i=0;i<runs;i++) {
            uint32_t length=u32(t->ctts.p+8+i*8), raw=u32(t->ctts.p+12+i*8);
            int64_t offset=t->ctts.p[0] ? (int64_t)(int32_t)raw : raw;
            if (length>total-(uint64_t)n) goto fail;
            for (uint32_t j=0;j<length;j++,n++) {
                if (offset>0 && samples[n].pts>INT64_MAX-offset) goto fail;
                samples[n].pts+=offset;
            }
        }
        if ((uint64_t)n!=total) goto fail;
    }
    if (t->stss.p) {
        if (t->stss.p[0]!=0 || !entries(t->stss,4,&runs)) goto fail;
        uint32_t prior=0;
        for (uint32_t i=0;i<runs;i++) {
            uint32_t sample=u32(t->stss.p+8+i*4);
            if (!sample || sample>total || sample<=prior) goto fail;
            samples[sample-1].key=true; prior=sample;
        }
    }
    int64_t shift=0;
    AVRational media_tb={1,(int)t->scale};
    if (t->elst.p) {
        uint32_t edits; unsigned stride=t->elst.p[0] ? 20 : 12;
        *reason="unsupported MOV/MP4 edit list; expected one media edit with optional leading empty edit";
        if (t->elst.p[0]>1 || !entries(t->elst,stride,&edits) || !edits || edits>2 || !movie_scale) goto fail;
        for (uint32_t i=0;i<edits;i++) {
            const unsigned char *p=t->elst.p+8+i*stride;
            uint64_t duration=stride==20 ? u64(p) : u32(p);
            int64_t media=stride==20 ? (int64_t)u64(p+8) : (int64_t)(int32_t)u32(p+4);
            uint32_t rate=u32(p+stride-4);
            if (rate!=0x00010000 || duration>INT64_MAX) goto fail;
            if (media==-1 && !i && edits==2) shift=av_rescale_q((int64_t)duration,(AVRational){1,(int)movie_scale},tb);
            else if (media>=0 && i==edits-1) {
                int64_t trim=av_rescale_q(media,media_tb,tb);
                if (trim==INT64_MIN || shift==INT64_MIN) goto fail;
                shift-=trim;
            } else goto fail;
        }
    }
    *reason="invalid MOV/MP4 presentation timestamps";
    for (uint64_t i=0;i<total;i++) {
        int64_t pts=av_rescale_q(samples[i].pts,media_tb,tb);
        if (pts==INT64_MIN || (shift>0 && pts>INT64_MAX-shift) || (shift<0 && pts<INT64_MIN-shift)) goto fail;
        samples[i].pts=pts+shift;
        samples[i].duration=av_rescale_q(samples[i].duration,media_tb,tb);
        if (samples[i].duration<=0 || samples[i].pts>INT64_MAX-samples[i].duration) goto fail;
    }
    qsort(samples,(size_t)total,sizeof(*samples),sample_compare);
    for (uint64_t i=1;i<total;i++) if (samples[i].pts<=samples[i-1].pts) goto fail;
    *out=samples; *count=(int)total; return true;
fail:
    free(samples); return false;
}

int video_timeline_read(const char *path, int track_id, AVRational tb,
                         VideoSample **samples, int *count, char *error, size_t size)
{
    *samples=NULL; *count=0;
    int fd=open(path,O_RDONLY); struct stat st;
    Track track={0}; const char *reason="invalid MOV/MP4 box structure";
    int result=-1; bool found=false; uint32_t movie_scale=0;
    if (fd<0 || fstat(fd,&st) || st.st_size<8 || tb.num<=0 || tb.den<=0) goto done;
    Box first;
    if (!box_at(fd,0,(uint64_t)st.st_size,&first)) { result=0; goto done; }
    if (first.type!=TAG('f','t','y','p') && first.type!=TAG('m','o','o','v') &&
        first.type!=TAG('m','d','a','t') && first.type!=TAG('f','r','e','e') && first.type!=TAG('w','i','d','e')) {
        result=0; goto done;
    }
    for (uint64_t pos=0;pos<(uint64_t)st.st_size;) {
        Box moov; if (!box_at(fd,pos,(uint64_t)st.st_size,&moov)) goto done;
        if (moov.type==TAG('m','o','o','v')) {
            for (uint64_t at=moov.data;at<moov.end;) {
                Box b; if (!box_at(fd,at,moov.end,&b)) goto done;
                if (b.type==TAG('m','v','h','d') && !scale_read(fd,b,&movie_scale)) goto done;
                if (b.type==TAG('t','r','a','k')) {
                    int id; if (!track_id_read(fd,b,&id)) goto done;
                    if (id==track_id) {
                        if (found || !track_read(fd,b,&track,0)) goto done;
                        found=true;
                    }
                }
                at=b.end;
            }
            break;
        }
        pos=moov.end;
    }
    if (!found) { reason="video track missing from MOV/MP4 sample tables"; goto done; }
    if (!samples_read(&track,movie_scale,tb,samples,count,&reason)) goto done;
    result=1;
done:
    if (result<0 && size) snprintf(error,size,"%s",reason);
    track_free(&track); if (fd>=0) close(fd);
    return result;
}
