/* Mock exact SDK leases; no devices, listeners, camera or SDK binary. */
#include "../../src/anyka/audio_backend.c"
#include <assert.h>

static unsigned last_gain,mock_rate=16000;
static int actual_rate(unsigned *out){*out=mock_rate;return 0;}
static unsigned open_calls,gain_calls,volume_calls,start_calls,get_calls,release_calls;
static int fail_stage,empty,release_error,release_not_clear,get_error;
static uint32_t next_bytes=1024,next_sequence=1;
static uint64_t next_ms=1000;
static const char *version_text="libplat_ai V1.11.02";
static const char *version(void){return version_text;}
static int ai_open(const struct ai_params *p,int *out)
{open_calls++;assert(p->rate==16000&&p->bits==16&&p->channels==1&&p->device==0);*out=0;return fail_stage==1?0x501:0;}
static int gain(unsigned handle,unsigned value)
{gain_calls++;assert(handle==0&&value<=8);last_gain=value;return fail_stage==2?0x502:0;}
static int volume(unsigned handle,int value)
{volume_calls++;assert(handle==0&&value==0);return fail_stage==3?0x503:0;}
static int start(unsigned handle)
{start_calls++;assert(handle==0);return fail_stage==4?0x504:0;}
static int get(unsigned handle,struct ai_frame *frame,int timeout)
{
    get_calls++;assert(handle==0&&timeout==0&&!frame->data);
    if(empty)return AI_EMPTY;
    frame->data=malloc(next_bytes?next_bytes:1);assert(frame->data);
    const uint8_t pattern[]={0x34,0x12,0xff,0x80};
    for(unsigned i=0;i<next_bytes;i++)((uint8_t *)frame->data)[i]=pattern[i%4];
    frame->bytes=next_bytes;frame->pts_ms=next_ms;frame->sequence=next_sequence++;
    return get_error?0x50f:0;
}
static int release(unsigned handle,struct ai_frame *frame)
{
    release_calls++;assert(handle==0&&frame->data);
    memset(frame->data,0xa5,frame->bytes);free(frame->data);
    if(!release_not_clear)memset(frame,0,sizeof *frame);
    return release_error?0x5ff:0;
}
static const struct audio_sdk sdk={version,ai_open,gain,volume,start,get,release,actual_rate};
static void reset(void)
{
    open_calls=gain_calls=volume_calls=start_calls=get_calls=release_calls=0;
    fail_stage=empty=release_error=release_not_clear=get_error=0;
    mock_rate=16000;next_bytes=1024;next_sequence=1;next_ms=1000;version_text="libplat_ai V1.11.02";
}
static void startup(void)
{
    reset();struct anyka_audio *s=NULL;
    version_text="libplat_ai V1.14.00";assert(open_bound(&s,&sdk)==-EPROTO&&!s&&!open_calls);
    version_text=NULL;assert(open_bound(&s,&sdk)==-EPROTO&&!s&&!open_calls);
    for(int stage=1;stage<=4;stage++){
        reset();fail_stage=stage;assert(open_bound(&s,&sdk)==-EIO&&s&&s->status.error==-EIO);
        assert(open_calls==1&&gain_calls==(stage>=2)&&volume_calls==(stage>=3)&&start_calls==(stage>=4));
        struct camera_media_frame out;assert(anyka_audio_read(s,1020000,&out)==-1&&!get_calls);
        free(s);s=NULL;
    }
}
static void steady_audio(void)
{
    reset();struct anyka_audio *s=NULL;assert(!open_bound(&s,&sdk));
    assert(open_bound(&s,&sdk)==-EINVAL&&open_calls==1);
    struct camera_media_frame out;
    empty=1;assert(!anyka_audio_read(s,1020000,&out)&&!release_calls&&!out.data);
    empty=0;assert(!anyka_audio_read(s,1020000,&out)&&release_calls==1&&!out.data&&!s->status.clock_valid);
    next_ms=1032;assert(anyka_audio_read(s,1052000,&out)==1&&release_calls==2&&out.size==1024&&out.pts_us==1032000);
    assert(out.data==s->output&&out.data[0]==0x12&&out.data[1]==0x34&&out.data[2]==0x80&&out.data[3]==0xff);
    assert(!s->lease_owned&&!s->lease.data&&s->status.clock_valid);
    next_ms=1064;assert(anyka_audio_read(s,1084000,&out)==1&&release_calls==3);
    struct anyka_audio_status status;assert(!anyka_audio_status(s,&status)&&status.samples==1536&&status.blocks==3&&status.last_source_ms==1064);
    free(s);
}
static void rejected_block(uint32_t bytes,uint64_t stamp,int error)
{
    reset();struct anyka_audio *s=NULL;assert(!open_bound(&s,&sdk));next_bytes=bytes;next_ms=stamp;
    struct camera_media_frame out;assert(anyka_audio_read(s,1020000,&out)==-1&&s->status.error==error&&!out.data&&release_calls==1);
    assert(anyka_audio_read(s,1052000,&out)==-1&&release_calls==1&&get_calls==1);free(s);
}
static void source_gaps(void)
{
    for(unsigned mode=0;mode<7;mode++){
        reset();struct anyka_audio *s=NULL;struct camera_media_frame out;
        assert(!open_bound(&s,&sdk));assert(!anyka_audio_read(s,1020000,&out));
        next_ms=1032;assert(anyka_audio_read(s,1052000,&out)==1);
        uint64_t now=1116000;
        if(mode==0)next_ms=1096; /* one 32ms block lost */
        if(mode==1){next_ms=2032;now=2052000;} /* bounded 1-second source interval */
        if(mode==2)next_ms=1031; /* backwards */
        if(mode==3){next_ms=1064;now=7000000;} /* stale source */
        if(mode==4){next_ms=2033;now=2053000;} /* over gap bound */
        if(mode==6)next_ms=1032; /* repeated raw source clock */
        if(mode==5)next_ms=1033; /* short first interval is bounded jitter, then sustained drift */
        int rc=anyka_audio_read(s,now,&out);
        if(mode==5){assert(rc==1&&out.pts_us>=1063000&&out.pts_us<=1064000);next_ms=1034;rc=anyka_audio_read(s,now,&out);}
        if(mode<2){
            assert(rc==1&&out.pts_us==next_ms*1000&&s->status.discontinuities==1);
            assert(s->status.last_gap_us==(next_ms-1032)*1000-32000);
            next_ms+=32;assert(anyka_audio_read(s,next_ms*1000+20000,&out)==1);
            assert(out.pts_us==next_ms*1000&&s->status.discontinuities==1&&!s->status.error);
        }else{
            assert(rc==-1&&!out.data&&s->status.error==-ERANGE);
            assert(s->status.candidate_source_ms==next_ms&&s->status.previous_source_ms==(mode==5?1033u:1032u));
            assert(s->status.candidate_now_us==now&&s->status.candidate_sequence==(mode==5?4u:3u));
        }
        assert(!s->lease_owned);free(s);
    }
}
static void failures(void)
{
    rejected_block(4098,1000,-EMSGSIZE);rejected_block(1023,1000,-EMSGSIZE);rejected_block(0,1000,-EMSGSIZE);
    rejected_block(1024,0,-ERANGE);rejected_block(1024,UINT64_MAX,-ERANGE);rejected_block(1024,1000000,-ERANGE);
    for(unsigned mode=0;mode<4;mode++){
        reset();struct anyka_audio *s=NULL;assert(!open_bound(&s,&sdk));struct camera_media_frame out;
        if(mode==0)get_error=1;
        if(mode==1)release_error=1;
        if(mode==2)release_not_clear=1;
        if(mode==3){assert(!anyka_audio_read(s,1020000,&out));next_ms=1064;}
        assert(anyka_audio_read(s,1084000,&out)==-1&&s->status.error);
        unsigned got=get_calls,released=release_calls;
        assert(anyka_audio_read(s,1116000,&out)==-1&&get_calls==got&&release_calls==released);
        if(mode==0){assert(!release_calls&&s->lease.data);free(s->lease.data);}
        free(s);
    }
}
static void irq_jitter_and_variable_blocks(void)
{
    reset();struct anyka_audio *s=NULL;struct camera_media_frame out;assert(!open_bound(&s,&sdk));
    assert(!anyka_audio_read(s,1020000,&out));next_ms=1032;assert(anyka_audio_read(s,1052000,&out)==1);
    next_ms=1048;assert(anyka_audio_read(s,1068000,&out)==1&&out.pts_us>=1063000&&out.pts_us<=1064000);
    assert(s->status.candidate_phase_us==-16000&&s->status.jittered_blocks==1&&!s->status.discontinuities);
    next_ms=1096;assert(anyka_audio_read(s,1116000,&out)==1&&out.pts_us>=1095000&&out.pts_us<=1096000&&!s->status.discontinuities);
    next_ms=1112;assert(anyka_audio_read(s,1132000,&out)==1);next_ms=1128;
    assert(anyka_audio_read(s,1148000,&out)==1);next_ms=1144;assert(anyka_audio_read(s,1164000,&out)==-1&&!s->lease_owned);free(s);
    reset();s=NULL;assert(!open_bound(&s,&sdk));assert(!anyka_audio_read(s,1020000,&out));
    next_bytes=512;next_ms=1032;assert(anyka_audio_read(s,1052000,&out)==1&&out.pts_us==1032000);
    next_bytes=1024;next_ms=1048;assert(anyka_audio_read(s,1068000,&out)==1&&out.pts_us==1048000&&s->status.candidate_expected_us==16000);
    next_bytes=512;next_ms=1080;assert(anyka_audio_read(s,1100000,&out)==1&&out.pts_us==1080000&&s->status.candidate_expected_us==32000);free(s);
    reset();s=NULL;assert(!open_bound(&s,&sdk));assert(!anyka_audio_read(s,1020000,&out));
    next_ms=1016;assert(!anyka_audio_read(s,1036000,&out)&&!s->status.clock_valid);
    next_ms=1064;assert(!anyka_audio_read(s,1084000,&out)&&!s->status.clock_valid);
    next_ms=1096;assert(anyka_audio_read(s,1116000,&out)==1&&s->status.clock_valid&&s->status.warmup_discarded==3);free(s);
    reset();s=NULL;assert(!open_bound(&s,&sdk));assert(!anyka_audio_read(s,1020000,&out));
    for(unsigned i=1;i<AI_WARMUP_MAX_BLOCKS;i++){next_ms+=16;assert(!anyka_audio_read(s,next_ms*1000+20000,&out)&&!s->status.clock_valid);}
    next_ms+=16;assert(anyka_audio_read(s,next_ms*1000+20000,&out)==-1&&!s->status.clock_valid&&!s->lease_owned);free(s);
    reset();s=NULL;assert(!open_bound(&s,&sdk));assert(!anyka_audio_read(s,1020000,&out));next_ms=1016;
    assert(anyka_audio_read(s,4000000,&out)==-1&&!s->status.clock_valid&&!s->lease_owned);free(s);
}
static void rate_validation(void)
{
    for(unsigned i=0;i<3;i++){
        reset();mock_rate=i==0?0:i==1?8000:48000;struct anyka_audio *s=NULL;
        assert(open_bound(&s,&sdk)==-EPROTO&&s&&s->status.error==-EPROTO&&!s->status.started);
        free(s);
    }
}
static void fractional_clock(void)
{
    reset();mock_rate=16025;struct anyka_audio *s=NULL;struct camera_media_frame out;assert(!open_bound(&s,&sdk));
    uint64_t previous=0,position=0;
    for(unsigned i=0;i<112700;i++){
        uint64_t actual=1000000u+(uint64_t)i*512u*624u/10u;
        if(i>2&&i%997==0)actual-=16000u;
        next_ms=actual/1000u;
        int rc=anyka_audio_read(s,actual+20000u,&out);
        if(i==0){assert(!rc);continue;}
        assert(rc==1&&out.sample_clock_rate==16025&&out.sample_position==position);
        assert(out.pts_us>previous&&out.pts_us<actual+32000u&&out.pts_us+32000u>actual);
        previous=out.pts_us;position+=512;
    }
    assert(!s->status.error&&!s->status.discontinuities&&s->status.jittered_blocks>100);free(s);
}
static void actual_clock_gap(void)
{
    reset();mock_rate=16025;struct anyka_audio *s=NULL;struct camera_media_frame out;
    assert(!open_bound(&s,&sdk));assert(!anyka_audio_read(s,1020000,&out));
    next_ms=1032;assert(anyka_audio_read(s,1052000,&out)==1);
    /* A real missing DMA period can round down below31.95ms. */
    next_ms=1095;assert(anyka_audio_read(s,1115000,&out)==1);
    assert(s->status.discontinuities==1&&out.sample_position>512&&out.pts_us==1095000);
    free(s);
}
static void controls(void)
{
 reset();struct anyka_audio *s=NULL;struct camera_media_frame out;assert(!open_bound(&s,&sdk)&&last_gain==5);
 assert(anyka_audio_control(s,2,50)==-EINVAL);assert(anyka_audio_control(s,1,101)==-EINVAL);
 assert(!anyka_audio_read(s,1020000,&out));assert(!anyka_audio_control(s,0,100));next_ms=1032;
 assert(anyka_audio_read(s,1052000,&out)==1&&last_gain==8&&out.pts_us==1032000);
 for(size_t i=0;i<out.size;i++)assert(!out.data[i]);
 assert(!anyka_audio_control(s,1,50));next_ms=1064;assert(anyka_audio_read(s,1084000,&out)==1&&last_gain==4&&out.data[0]==0x12);free(s);
}
int main(void)
{
    startup();steady_audio();source_gaps();failures();controls();irq_jitter_and_variable_blocks();rate_validation();fractional_clock();actual_clock_gap();
    puts("Anyka audio: exact version/startup bounds, nonblocking empty, PCM lease copy/release/endian, clock/cadence guards and sticky faults PASS");
    return 0;
}
