/* Execute the actual static serializer without sockets, devices or services. */
#define clock_gettime test_clock_gettime
#include "../camera_rtsp.c"
#include <assert.h>
static unsigned keyframe_requests;
void camera_request_keyframe(void){keyframe_requests++;}
static time_t wall_seconds=1700000000;
int test_clock_gettime(clockid_t id,struct timespec *t)
{t->tv_sec=id==CLOCK_REALTIME?wall_seconds:20;t->tv_nsec=0;return 0;}
static void corrected_session(void)
{
    struct camera_media_provider provider={0};provider.description.audio_rate=8000;
    struct server s={0};s.provider=&provider;
    struct client old={0},fresh={0};old.video.setup=fresh.video.setup=1;old.playing=fresh.playing=1;old.need_idr=fresh.need_idr=1;
    old.tx=calloc(1,TX_CAP);fresh.tx=calloc(1,TX_CAP);assert(old.tx&&fresh.tx);
    const uint8_t idr[]={0,0,0,1,0x65,0x80};
    struct camera_media_frame frame={.data=idr,.size=sizeof idr,.pts_us=20000000,.keyframe=1};
    assert(!video_frame(&s,&old,&frame));assert(old.tx[5]==200);assert(old.tx[24]==0&&old.tx[28]==0);int64_t before=old.realtime_offset;
    wall_seconds+=3600;assert(!video_frame(&s,&fresh,&frame));
    assert(fresh.realtime_offset==before+INT64_C(3600000000));
    frame.pts_us+=66666;assert(!video_frame(&s,&old,&frame));assert(old.realtime_offset==before);
    free(old.tx);free(fresh.tx);
}
static uint32_t word(const uint8_t *p)
{return(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void audio_sample_clock(void)
{
    struct camera_media_provider provider={0};provider.description.audio_rate=16000;provider.description.audio_codec=CAMERA_AUDIO_L16;
    struct server s={0};s.provider=&provider;
    struct client c={0};c.playing=c.audio.setup=c.video.setup=1;c.origin=10000000;
    c.realtime_offset=INT64_C(1700000000000000);c.audio.base=UINT32_MAX-100;c.video.base=123;
    c.tx=calloc(1,TX_CAP);assert(c.tx);
    /* Before any audio the initial SR still has the nominal SDP clock. */
    assert(!sender_report(&s,&c,&c.audio,c.origin+1000,1));assert(word(c.tx+20)==c.audio.base+16);
    uint8_t pcm[1024]={0};
    struct camera_media_frame f={.data=pcm,.size=sizeof pcm,.pts_us=c.origin+100000,.sample_position=9000,.sample_clock_rate=16025};
    uint32_t first=c.audio.base+1600,previous=first;
    for(unsigned i=0;i<4;i++){
        c.queued=c.sent=0;
        /* 512-sample SDK blocks have coarse, irregular 31/32ms source PTS. */
        const unsigned offset[]={0,31000,63000,94000};
        f.pts_us=c.origin+100000+offset[i];f.sample_position=9000+i*512;
        assert(!audio_frame(&s,&c,&f));assert(c.queued==sizeof pcm+16);
        uint32_t stamp=word(c.tx+8);assert(stamp==first+i*512);
        if(i)assert((uint32_t)(stamp-previous)==512);
        previous=stamp;
    }
    /* Source-reported lost samples remain a real gap in the RTP timeline. */
    c.queued=c.sent=0;f.sample_position+=1024;f.pts_us+=64000;
    assert(!audio_frame(&s,&c,&f));assert(word(c.tx+8)==previous+1024);
    uint32_t last=word(c.tx+8);
    uint64_t now=f.pts_us+2000000;
    c.queued=c.sent=0;assert(!sender_report(&s,&c,&c.audio,now,0));
    assert(word(c.tx+20)==last+32050&&word(c.tx+20)!=last+32000);
    uint32_t ntp_seconds=word(c.tx+12),ntp_fraction=word(c.tx+16);
    c.queued=c.sent=0;c.video.packets=1;assert(!sender_report(&s,&c,&c.video,now,0));
    assert(word(c.tx+12)==ntp_seconds&&word(c.tx+16)==ntp_fraction);
    /* Invalid source metadata closes the affected client before serialization. */
    c.queued=c.sent=0;f.sample_position++;f.pts_us++;
    assert(audio_frame(&s,&c,&f)==-1&&!c.queued); /* overlapping block */
    f.sample_position=UINT64_MAX;assert(audio_frame(&s,&c,&f)==-1);
    f.sample_position=20000;f.sample_clock_rate=16000;assert(audio_frame(&s,&c,&f)==-1);
    f.sample_clock_rate=0;assert(audio_frame(&s,&c,&f)==-1);
    free(c.tx);
    /* Legacy PCMA providers retain the nominal clock path. */
    memset(&c,0,sizeof c);c.playing=c.audio.setup=1;c.origin=10000000;c.tx=calloc(1,TX_CAP);assert(c.tx);
    provider.description.audio_codec=CAMERA_AUDIO_PCMA;provider.description.audio_rate=8000;
    f.sample_clock_rate=0;f.size=160;f.pts_us=c.origin+20000;
    assert(!audio_frame(&s,&c,&f)&&word(c.tx+8)==160);
    c.queued=c.sent=0;f.pts_us+=20000;assert(!audio_frame(&s,&c,&f)&&word(c.tx+8)==320);
    free(c.tx);
}
static void verify(int video,unsigned rate)
{
    struct camera_media_provider provider={0};provider.description.audio_rate=rate;
    struct server s={0};s.provider=&provider;
    struct client c={0};c.realtime_offset=INT64_C(1700000000000000);c.origin=10000000;c.tx=calloc(1,TX_CAP);assert(c.tx);
    struct track *t=video?&c.video:&c.audio;
    t->setup=1;t->base=UINT32_MAX-100;t->ssrc=0x1234;t->packets=71;t->octets=123456;
    t->last_pts=c.origin+100000;t->last_stamp=t->base+123; /* deliberately stale */
    uint64_t times[2]={c.origin+5000789,c.origin+6000789};
    uint32_t previous=0;
    for(unsigned i=0;i<2;i++) {
        c.queued=c.sent=0;
        assert(!sender_report(&s,&c,t,times[i],0)&&c.queued==76);
        uint64_t wall=(uint64_t)c.realtime_offset+times[i];
        assert(word(c.tx+12)==(uint32_t)(wall/1000000+2208988800u));
        assert(word(c.tx+16)==(uint32_t)((wall%1000000)*(UINT64_C(1)<<32)/1000000));
        uint32_t stamp=word(c.tx+20),clock_rate=video?90000:rate;
        assert(stamp==t->base+(uint32_t)((times[i]-c.origin)*clock_rate/1000000));
        assert(stamp!=t->last_stamp);
        if(i)assert((uint32_t)(stamp-previous)==clock_rate);
        previous=stamp;
        assert(word(c.tx+24)==71&&word(c.tx+28)==123456);
        assert(t->last_pts==c.origin+100000&&t->last_stamp==t->base+123);
    }
    c.queued=0;assert(!sender_report(&s,&c,t,times[1]+999999,0)&&!c.queued);
    /* Poll's earlier timestamp must not underflow last_sr or close a client
     * immediately after its initial report used a fresher clock. */
    c.queued=0;assert(!sender_report(&s,&c,t,times[1]-1,0)&&!c.queued&&t->last_sr==times[1]);
    assert(!sender_report(&s,&c,t,times[1],0)&&!c.queued&&t->last_sr==times[1]);
    free(c.tx);
}
static void keyframe_on_authenticated_play(void)
{
 struct server s={0};s.cfg.path="/native";strcpy(s.authorization,"Basic dGVzdDp0ZXN0");
 struct client c={0};c.video.setup=1;strcpy(c.session,"0123456789abcdef0123456789abcdef");c.tx=malloc(TX_CAP);assert(c.tx);
 char unauthorized[]="PLAY rtsp://127.0.0.1/native RTSP/1.0\r\nCSeq: 1\r\nSession: 0123456789abcdef0123456789abcdef\r\n\r\n";
 assert(!request(&s,&c,unauthorized)&&!keyframe_requests&&!c.playing);c.queued=0;
 char invalid[]="PLAY rtsp://127.0.0.1/native RTSP/1.0\r\nCSeq: 2\r\nAuthorization: Basic dGVzdDp0ZXN0\r\nSession: invalid\r\n\r\n";
 assert(!request(&s,&c,invalid)&&!keyframe_requests&&!c.playing);c.queued=0;
 char valid[]="PLAY rtsp://127.0.0.1/native RTSP/1.0\r\nCSeq: 3\r\nAuthorization: Basic dGVzdDp0ZXN0\r\nSession: 0123456789abcdef0123456789abcdef\r\n\r\n";
 assert(!request(&s,&c,valid)&&keyframe_requests==1&&c.playing&&c.need_idr);
 assert(!request(&s,&c,valid)&&keyframe_requests==1);free(c.tx);
}
int main(void)
{
    verify(1,0);verify(0,8000);verify(0,16000);verify(0,32000);verify(0,48000);
    corrected_session();
    audio_sample_clock();
    keyframe_on_authenticated_play();
    puts("RTCP sender report:5 clocks passed; stale-media time, wrap and no-media advance");return 0;
}
