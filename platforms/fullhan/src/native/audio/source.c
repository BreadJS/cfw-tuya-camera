#define _GNU_SOURCE
#include "source.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
struct block { uint8_t bytes[FH_AUDIO_SOURCE_SAMPLES];size_t count;uint64_t pts_us; };
struct fh_audio_source {
    pthread_t thread;
    pthread_mutex_t mutex;
    atomic_int phase;
    atomic_uint controls;
    struct fh_audio audio;
    struct fh_audio_lease lease;
    struct fh_audio_source_policy policy;
    struct fh_audio_io io;
    struct fh_audio_source_status status;
    struct block queue[FH_AUDIO_SOURCE_QUEUE],output;
    unsigned head,count,have_pts;
    uint64_t previous_ns;
};
uint8_t fh_audio_pcma(int16_t sample)
{
    /* ITU G.711 A-law: signed16 linear input, eight logarithmic segments. */
    int pcm=sample;unsigned mask;
    if(pcm>=0)mask=0xd5;else {mask=0x55;pcm=-pcm-1;}
    unsigned segment=0;
    for(unsigned limit=0xff;segment<8 && (unsigned)pcm>limit;limit=(limit<<1)|1)++segment;
    if(segment>=8)return (uint8_t)(0x7f^mask);
    unsigned value=segment<<4;
    value|=(unsigned)pcm>>(segment<2?4:segment+3)&15U;
    return (uint8_t)(value^mask);
}
static void phase(struct fh_audio_source *s,enum fh_audio_source_phase p)
{ atomic_store_explicit(&s->phase,p,memory_order_relaxed); }
static void fault(struct fh_audio_source *s,int rc)
{
    pthread_mutex_lock(&s->mutex);
    s->status.failed_phase=(enum fh_audio_source_phase)atomic_load_explicit(&s->phase,memory_order_relaxed);
    s->status.error=rc>0?-EIO:rc;phase(s,FH_AUDIO_SOURCE_FAULT);s->status.done=1;
    pthread_mutex_unlock(&s->mutex);
}
static void *produce(void *opaque)
{
    struct fh_audio_source *s=opaque;uint32_t words[7];uint8_t pcm[512];int rc;
    unsigned consecutive_zero=0,warmup_blocks=0;
    rc=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);if(rc){fault(s,-rc);return NULL;}
    rc=fh_audio_init(&s->audio,&s->lease,&s->io);if(rc){fault(s,rc);return NULL;}
    phase(s,FH_AUDIO_SOURCE_OPEN);
    rc=fh_audio_open(&s->audio);if(rc){fault(s,rc);return NULL;}
    fh_audio_stock_config(words);phase(s,FH_AUDIO_SOURCE_CONFIGURE);
    rc=fh_audio_configure(&s->audio,words);
    pthread_mutex_lock(&s->mutex);
    memcpy(s->status.config_request,s->audio.config_request,32);
    memcpy(s->status.config_reply,s->audio.config_reply,32);
    pthread_mutex_unlock(&s->mutex);
    if(rc){fault(s,rc);return NULL;}
    phase(s,FH_AUDIO_SOURCE_ENABLE);
    rc=fh_audio_start(&s->audio);if(rc){fault(s,rc);return NULL;}
    rc=fh_audio_bind_producer(&s->audio);if(rc){fault(s,rc);return NULL;}
    for(;;) {
        struct fh_audio_frame frame;struct block block;uint64_t ns;
        phase(s,FH_AUDIO_SOURCE_READ);
        rc=fh_audio_read(&s->audio,pcm,sizeof pcm,1,&frame);
        if(rc){fault(s,rc);return NULL;}
        if(!frame.bytes) {
            pthread_mutex_lock(&s->mutex);++s->status.zero_blocks;pthread_mutex_unlock(&s->mutex);
            if(++consecutive_zero>=32){fault(s,-ENODATA);return NULL;}
            continue;
        }
        consecutive_zero=0;
        if(!frame.has_raw_pts || frame.bytes%2 || frame.bytes>sizeof pcm){fault(s,-EPROTO);return NULL;}
        uint64_t raw=(uint64_t)frame.raw_pts[0]|(uint64_t)frame.raw_pts[1]<<32;
        rc=s->policy.clock.raw_to_ns(s->policy.clock.opaque,raw,&ns);
        if(rc){fault(s,rc);return NULL;}
        if(s->have_pts && (ns<=s->previous_ns || ns/1000<=s->previous_ns/1000)) {
            fault(s,-ERANGE);return NULL;
        }
        s->have_pts=1;s->previous_ns=ns;
        block.count=frame.bytes/2;block.pts_us=ns/1000;
        /* Actual stock8k microphone exhibits a large analog/DC startup
         * transient. Discard64 valid blocks without rebasing source time. */
        if(warmup_blocks<FH_AUDIO_SOURCE_WARMUP_BLOCKS) {
            ++warmup_blocks;
            pthread_mutex_lock(&s->mutex);
            ++s->status.warmup_blocks;s->status.warmup_samples+=block.count;
            memcpy(s->status.last_raw_pts,frame.raw_pts,sizeof frame.raw_pts);
            pthread_mutex_unlock(&s->mutex);
            continue;
        }
        unsigned controls=atomic_load_explicit(&s->controls,memory_order_relaxed);
        for(size_t i=0;i<block.count;i++) {
            unsigned u=(unsigned)pcm[2*i]|(unsigned)pcm[2*i+1]<<8;
            int sample=u<32768U?(int)u:(int)u-65536;
            sample=(controls&0x10000U)?sample*(int)(controls&0xffffU)/100:0;
            if(sample>32767)sample=32767;
            if(sample< -32768)sample= -32768;
            block.bytes[i]=fh_audio_pcma((int16_t)sample);
        }
        pthread_mutex_lock(&s->mutex);
        if(s->count==FH_AUDIO_SOURCE_QUEUE) {
            s->head=(s->head+1)%FH_AUDIO_SOURCE_QUEUE;--s->count;++s->status.dropped;
        }
        unsigned tail=(s->head+s->count)%FH_AUDIO_SOURCE_QUEUE;
        s->queue[tail]=block;++s->count;++s->status.blocks;s->status.samples+=block.count;
        memcpy(s->status.last_raw_pts,frame.raw_pts,sizeof frame.raw_pts);
        pthread_mutex_unlock(&s->mutex);
    }
}
int fh_audio_source_start(struct fh_audio_source **out,const struct fh_audio_lease *lease,
    const struct fh_audio_source_policy *policy,const struct fh_audio_io *io)
{
    if(!out || *out || !lease || !lease->device || !lease->check || !policy)return -EINVAL;
    if(policy->cold_boot_verified!=1 || policy->pcm_s16le_verified!=1 ||
       policy->pts_low_high_verified!=1 || !policy->clock.raw_to_ns)return -ENOTSUP;
    if(!io)io=fh_audio_linux_io();
    if(!io->open||!io->close||!io->fstat||!io->lstat||!io->ioctl||!io->read||!io->pointer32)return -EINVAL;
    struct fh_audio_source *s=calloc(1,sizeof *s);if(!s)return -ENOMEM;
    s->lease=*lease;s->policy=*policy;s->io=*io;s->audio.fd=-1;
    atomic_init(&s->phase,FH_AUDIO_SOURCE_INIT);
    atomic_init(&s->controls,0x10000U|100U);
    int rc=pthread_mutex_init(&s->mutex,NULL);if(rc){free(s);return -rc;}
    pthread_attr_t attr;rc=pthread_attr_init(&attr);
    if(rc){pthread_mutex_destroy(&s->mutex);free(s);return -rc;}
    rc=pthread_attr_setstacksize(&attr,65536);
    if(!rc)rc=pthread_create(&s->thread,&attr,produce,s);
    pthread_attr_destroy(&attr);
    if(rc){pthread_mutex_destroy(&s->mutex);free(s);return -rc;}
    *out=s;return 0;
}
int fh_audio_source_next(void *opaque,uint64_t now_us,struct camera_media_frame *frame)
{
    struct fh_audio_source *s=opaque;(void)now_us;
    if(!s||!frame)return -1;
    memset(frame,0,sizeof *frame);
    int rc=pthread_mutex_trylock(&s->mutex);if(rc)return rc==EBUSY?0:-1;
    if(!s->count) {rc=s->status.done?-1:0;pthread_mutex_unlock(&s->mutex);return rc;}
    s->output=s->queue[s->head];s->head=(s->head+1)%FH_AUDIO_SOURCE_QUEUE;
    --s->count;++s->status.delivered;pthread_mutex_unlock(&s->mutex);
    *frame=(struct camera_media_frame){.data=s->output.bytes,.size=s->output.count,.pts_us=s->output.pts_us};return 1;
}
int fh_audio_source_controls(struct fh_audio_source *s,unsigned enabled,unsigned gain)
{
    if(!s||enabled>1||gain>400)return -EINVAL;
    atomic_store_explicit(&s->controls,(enabled?0x10000U:0U)|gain,memory_order_relaxed);
    return 0;
}
int fh_audio_source_status(struct fh_audio_source *s,struct fh_audio_source_status *out)
{
    if(!s||!out)return -EINVAL;
    int rc=pthread_mutex_trylock(&s->mutex);if(rc)return -rc;
    *out=s->status;out->queued=s->count;
    out->phase=(enum fh_audio_source_phase)atomic_load_explicit(&s->phase,memory_order_relaxed);
    pthread_mutex_unlock(&s->mutex);return 0;
}
