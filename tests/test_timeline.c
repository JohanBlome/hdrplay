#include "audio_internal.h"
#include "video_timeline.h"
#include "decoder.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int g_verbose;
static int fails;
#define CHECK(c, name) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",name); fails++; } } while (0)
int main(int argc,char **argv)
{
    if (argc!=3) return 2;
    int64_t pts[1024],duration[1024]; int keys[1024]; int count=0;
    FILE *ref=fopen(argv[2],"r"); if (!ref) return 2;
    long long p,d; int key;
    while (count<1024 && fscanf(ref,"%lld %lld %d",&p,&d,&key)==3) { pts[count]=p; keys[count]=key; duration[count++]=d; }
    fclose(ref);
    const char *paths[]={argv[1]};
    uint64_t began=SDL_GetTicks();
    Audio *a=audio_open(paths,1,0,0,false,false,NULL);
    CHECK(a!=NULL,"MOV/MP4 audio inspection opens"); if (!a) return 1;
    CHECK(a->inputs[0].box_timeline,"timeline uses sample tables");
    CHECK(a->inputs[0].n_frames==count,"sample count matches decoded frames");
    for (int i=0;i<count && i<a->inputs[0].n_frames;i++) {
        CHECK(a->inputs[0].frames[i].pts==pts[i],"sample-table PTS matches full FFmpeg decode");
        CHECK(duration[i]<=0 || a->inputs[0].frames[i].duration==duration[i],"sample-table duration matches decoded frame duration");
        CHECK(a->inputs[0].frames[i].key==(keys[i]!=0),"sync-sample flags match decoded keyframes");
    }
    AudioGridTick ticks[512]; AudioGridLevel level;
    audio_timeline_grid(a,0,0,1200,1000,ticks,512,&level); CHECK(level==AUDIO_GRID_MINUTES,"minute divisions");
    audio_timeline_grid(a,0,0,300,1000,ticks,512,&level); CHECK(level==AUDIO_GRID_SECONDS_30,"30-second divisions");
    audio_timeline_grid(a,0,0,60,1000,ticks,512,&level); CHECK(level==AUDIO_GRID_SECONDS_10,"10-second divisions");
    audio_timeline_grid(a,0,0,12,1000,ticks,512,&level); CHECK(level==AUDIO_GRID_KEYS,"keyframe divisions");
    int n=audio_timeline_grid(a,0,.236081,.018528,1000,ticks,512,&level);
    CHECK(level==AUDIO_GRID_FRAMES && n>0,"narrow view includes its enclosing frame");
    if (n) {
        CHECK(ticks[0].time<=.236081 && ticks[0].end>.236081,"enclosing frame covers viewport start");
        CHECK(!ticks[0].boundary,"enclosing frame is not a fabricated boundary at viewport edge");
        CHECK(ticks[0].key==a->inputs[0].frames[ticks[0].ordinal].key,"keyframe color flag matches sample table");
    }
    for (int i=0;i<100;i++) audio_timeline_grid(a,0,.236081,.018528,1000,ticks,512,&level);
    printf("metadata and zoom queries: %.3f s (%d frames)\n",(SDL_GetTicks()-began)/1000.0,count);
    Decoder dec;
    if (!decoder_open(&dec,argv[1])) { audio_close(a); return 1; }
    dec.validate_frame=audio_validate_frame; dec.validation_opaque=a; dec.validation_input=0;
    int seen=0,r;
    while ((r=decoder_next_frame(&dec))>0) seen++;
    CHECK(r==0 && seen==count,"normal playback validates every sample timestamp and ordinal");
    CHECK(decoder_seek_start(&dec),"rewind for mismatch test");
    SDL_LockMutex(a->mutex); a->inputs[0].frames[0].pts++; SDL_UnlockMutex(a->mutex);
    CHECK(decoder_next_frame(&dec)<0,"decoder stops when sample table differs from playback");
    AudioInfo info; audio_info(a,&info);
    CHECK(info.failed && info.timeline_mismatch,"timestamp mismatch is an explicit fatal error");
    decoder_close(&dec); audio_close(a);
    printf("timeline: %s\n",fails ? "FAILED":"passed");
    return fails!=0;
}
