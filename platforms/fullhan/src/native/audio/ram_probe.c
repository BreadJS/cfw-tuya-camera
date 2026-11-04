#define _GNU_SOURCE
#include "ram_probe.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

struct fh_audio_ram_job {
    pthread_t thread;
    atomic_int done,phase;
    struct fh_audio_lease lease;
    struct fh_audio audio;
    int pae_fd,data_fd,log_fd;
    unsigned rate_hz,warm_disable;
    struct fh_audio_ram_status final;
    uint8_t pcm[FH_AUDIO_RAM_PAYLOAD_BYTES];
};
static void phase(struct fh_audio_ram_job *j,enum fh_audio_ram_phase p)
{ atomic_store_explicit(&j->phase,p,memory_order_relaxed); }
static int monotonic(uint64_t *ns)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return -errno;
    *ns=(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;return 0;
}
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int current_pts(struct fh_audio_ram_job *j,uint64_t *pts)
{
    uint8_t wire[8]={0};
    int rc=j->lease.check(j->lease.opaque,&j->audio,FH_AUDIO_LEASE_READ);
    if(rc)return rc>0 ? -EIO : rc;
    rc=ioctl(j->pae_fd,0xc0085041UL,wire);
    if(rc)return rc==-1 ? -errno : rc>0 ? -EIO : rc;
    *pts=(uint64_t)le32(wire)|(uint64_t)le32(wire+4)<<32;return 0;
}
static int write_all(int fd,const uint8_t *p,size_t bytes,uint32_t *written)
{
    while(bytes) {
        ssize_t n=write(fd,p,bytes);
        if(n<0){if(errno==EINTR)continue;return -errno;}
        if(!n)return -EIO;
        p+=(size_t)n;bytes-=(size_t)n;*written+=(uint32_t)n;
    }
    return 0;
}
static void rpc_snapshot(struct fh_audio_ram_job *j,const char *when)
{
    uint8_t buffer[1024];
    size_t total=0;
    uint32_t logged=0;
    int result=0,close_result=0;
    int fd=open("/proc/driver/rtvbus",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) {
        (void)dprintf(j->log_fd,"AUDIO_RAM_RTVBUS when=%s result=%d nonfatal=yes\n",when,-errno);
        return;
    }
    (void)dprintf(j->log_fd,"AUDIO_RAM_RTVBUS_BEGIN when=%s max_bytes=8192\n",when);
    while(total<8192) {
        size_t ask=8192-total;
        if(ask>sizeof(buffer))ask=sizeof(buffer);
        ssize_t n=read(fd,buffer,ask);
        if(n<0){if(errno==EINTR)continue;result=-errno;break;}
        if(!n)break;
        total+=(size_t)n;
        result=write_all(j->log_fd,buffer,(size_t)n,&logged);
        if(result)break;
    }
    if(close(fd)<0)close_result=-errno; /* Linux close is not retried. */
    (void)dprintf(j->log_fd,"\nAUDIO_RAM_RTVBUS_END when=%s bytes=%zu logged=%u limit_reached=%u result=%d close_result=%d nonfatal=yes\n",
                  when,total,logged,total==8192,result,close_result);
}
static void config_dump(struct fh_audio_ram_job *j,int rc)
{
    static const char hex[]="0123456789abcdef";
    char request[65],reply[65];
    for(unsigned i=0;i<32;++i) {
        request[2*i]=hex[j->audio.config_request[i]>>4];
        request[2*i+1]=hex[j->audio.config_request[i]&15];
        reply[2*i]=hex[j->audio.config_reply[i]>>4];
        reply[2*i+1]=hex[j->audio.config_reply[i]&15];
    }
    request[64]=reply[64]=0;
    (void)dprintf(j->log_fd,"AUDIO_RAM_CONFIG candidate_rate=%u rc=%d configure_attempted=%u last_error=%d request_hex=%s reply_hex=%s\n",
                  j->rate_hz,rc,j->audio.configure_attempted,j->audio.last_error,request,reply);
}
static void statistics(struct fh_audio_ram_job *j)
{
    struct fh_audio_ram_status *s=&j->final;
    while(s->complete_samples*2U+1U<s->bytes) {
        size_t p=s->complete_samples*2U;
        uint32_t u=(uint32_t)j->pcm[p]|(uint32_t)j->pcm[p+1]<<8;
        int32_t sample=u<=32767U ? (int32_t)u : (int32_t)u-65536;
        if(!s->complete_samples || sample<s->minimum)s->minimum=sample;
        if(!s->complete_samples || sample>s->maximum)s->maximum=sample;
        s->sum+=sample;s->sum_squares+=(uint64_t)((int64_t)sample*sample);
        ++s->complete_samples;
    }
    s->odd_tail_bytes=s->bytes&1U;
}
static void *run(void *opaque)
{
    struct fh_audio_ram_job *j=opaque;
    struct fh_audio_ram_status *s=&j->final;
    uint32_t config[7];
    uint64_t start=0,now=0;
    int rc=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);
    if(rc){s->result=-rc;goto done;}
    phase(j,FH_AUDIO_RAM_FILES);
    struct statfs fs;
    if(statfs("/tmp",&fs)){s->result=-errno;goto done;}
    if((unsigned long)fs.f_type!=0x01021994UL && (unsigned long)fs.f_type!=0x858458f6UL) {
        s->result=-ENOTSUP;goto done;
    }
    j->data_fd=open("/tmp/native-audio.s16le",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(j->data_fd<0){s->result=-errno;goto done;}
    j->log_fd=open("/tmp/native-audio.log",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(j->log_fd<0){s->result=-errno;goto done;}
    phase(j,FH_AUDIO_RAM_INIT);
    rc=fh_audio_init(&j->audio,&j->lease,NULL);
    if(rc){s->result=rc;goto done;}
    phase(j,FH_AUDIO_RAM_OPEN);
    if((rc=fh_audio_open(&j->audio))){s->result=rc;goto done;}
    if(j->warm_disable) {
        phase(j,FH_AUDIO_RAM_WARM_DISABLE);
        rpc_snapshot(j,"before-warm-disable");
        rc=fh_audio_prepare_exclusive_input(&j->audio);
        rpc_snapshot(j,"after-warm-disable");
        (void)dprintf(j->log_fd,"AUDIO_RAM_WARM_DISABLE rc=%d attempted=%u ack=%u state=%u enable_attempted=%u normal_disable_attempted=%u full_quiescence_claim=no\n",
                      rc,j->audio.exclusive_prepare_attempted,j->audio.exclusive_prepare_ack,
                      (unsigned)j->audio.state,j->audio.enable_attempted,j->audio.disable_attempted);
        if(rc){s->result=rc;goto done;}
    }
    fh_audio_stock_config(config);config[1]=j->rate_hz;
    phase(j,FH_AUDIO_RAM_CONFIGURE);
    rpc_snapshot(j,"before-configure");
    rc=fh_audio_configure(&j->audio,config);
    rpc_snapshot(j,"after-configure");
    config_dump(j,rc);
    if(rc){s->result=rc;goto done;}
    phase(j,FH_AUDIO_RAM_ENABLE);
    if((rc=fh_audio_start(&j->audio))){s->result=rc;goto done;}
    phase(j,FH_AUDIO_RAM_BIND);
    if((rc=fh_audio_bind_producer(&j->audio))){s->result=rc;goto done;}
    if((rc=monotonic(&start))){s->result=rc;goto done;}
    (void)dprintf(j->log_fd,"AUDIO_RAM_BEGIN candidate_rate=%u channels=1 width=16 period=256 payload_limit=64000 soft_deadline_ns=2000000000 block_limit=256 monotonic_start_ns=%" PRIu64 " timestamp_units_unproven=yes\n",j->rate_hz,start);
    for(;;) {
        struct fh_audio_frame frame;
        uint64_t before=0,after=0;
        if((rc=monotonic(&now))){s->result=rc;break;}
        s->elapsed_ns=now-start;
        if(s->bytes>=FH_AUDIO_RAM_PAYLOAD_BYTES){s->end=FH_AUDIO_RAM_PAYLOAD_LIMIT;break;}
        if(s->blocks>=FH_AUDIO_RAM_MAX_BLOCKS){s->end=FH_AUDIO_RAM_BLOCK_LIMIT;break;}
        if(s->elapsed_ns>=UINT64_C(2000000000)){s->end=FH_AUDIO_RAM_SOFT_DEADLINE;break;}
        /* Never request an undersized tail that the audio API cannot fill. */
        if(FH_AUDIO_RAM_PAYLOAD_BYTES-s->bytes<j->audio.request_bytes){s->end=FH_AUDIO_RAM_PAYLOAD_LIMIT;break;}
        phase(j,FH_AUDIO_RAM_CLOCK_BEFORE);
        if((rc=current_pts(j,&before))){s->result=rc;break;}
        phase(j,FH_AUDIO_RAM_BLOCKING_READ);
        size_t remaining=FH_AUDIO_RAM_PAYLOAD_BYTES-s->bytes;
        size_t capacity=remaining<FH_AUDIO_MAX_BYTES ? remaining : FH_AUDIO_MAX_BYTES;
        rc=fh_audio_read(&j->audio,j->pcm+s->bytes,capacity,1,&frame);
        if(rc){s->result=rc;break;}
        uint32_t previous=s->bytes;
        s->bytes+=(uint32_t)frame.bytes;++s->blocks;
        if(!frame.bytes)++s->zero_blocks;
        statistics(j);
        phase(j,FH_AUDIO_RAM_CLOCK_AFTER);
        int after_rc=current_pts(j,&after);
        int time_rc=monotonic(&now);
        if(!time_rc)s->elapsed_ns=now-start;
        if(dprintf(j->log_fd,"AUDIO_RAM_BLOCK index=%u bytes=%zu total=%u raw0=%" PRIu32 " raw1=%" PRIu32 " has_raw=%u pae_before=%" PRIu64 " pae_after=%" PRIu64 " pae_after_rc=%d elapsed_ns=%" PRIu64 "\n",
                   s->blocks,frame.bytes,s->bytes,frame.raw_pts[0],frame.raw_pts[1],frame.has_raw_pts,
                   before,after,after_rc,s->elapsed_ns)<0){s->result=-errno;break;}
        phase(j,FH_AUDIO_RAM_WRITE);
        rc=write_all(j->data_fd,j->pcm+previous,frame.bytes,&s->written_bytes);
        if(rc){s->result=rc;break;}
        if(after_rc){s->result=after_rc;break;}
        if(time_rc){s->result=time_rc;break;}
    }
done:
    /* Deliberately retain enabled hardware, private scratch, file descriptors,
     * job heap and joinable thread resources for external recovery/reset. */
    s->last_operation=(enum fh_audio_ram_phase)atomic_load_explicit(&j->phase,memory_order_relaxed);
    if(start && !monotonic(&now))s->elapsed_ns=now-start;
    if(j->log_fd>=0)
        (void)dprintf(j->log_fd,"AUDIO_RAM_END result=%d end=%u phase=%d bytes=%u written_bytes=%u blocks=%u zero_blocks=%u samples_s16le_candidate=%u odd_tail=%u min=%" PRId32 " max=%" PRId32 " sum=%" PRId64 " sum_squares=%" PRIu64 " elapsed_ns=%" PRIu64 " audio_fd=%d enable_attempted=%u enable_ack=%u hardware_stop=no resources_retained=yes\n",
                      s->result,(unsigned)s->end,(int)s->last_operation,
                      s->bytes,s->written_bytes,s->blocks,s->zero_blocks,s->complete_samples,s->odd_tail_bytes,
                      s->minimum,s->maximum,s->sum,s->sum_squares,s->elapsed_ns,
                      j->audio.fd,j->audio.enable_attempted,j->audio.enable_ack);
    s->done=1;s->phase=FH_AUDIO_RAM_COMPLETE;
    phase(j,FH_AUDIO_RAM_COMPLETE);
    atomic_store_explicit(&j->done,1,memory_order_release);
    return NULL;
}
int fh_audio_ram_probe_start_ex(struct fh_audio_ram_job **out,const struct fh_audio_lease *lease,int pae_fd,
                                 unsigned rate_hz,unsigned warm_disable)
{
    struct fh_audio_ram_job *j;
    pthread_attr_t attr;
    int rc;
    if(!out || *out || !lease || !lease->device || !lease->check || pae_fd<0)return -EINVAL;
    if(rate_hz!=8000 && rate_hz!=16000 && rate_hz!=32000)return -ENOTSUP;
    if(warm_disable>1)return -EINVAL;
    j=calloc(1,sizeof(*j));if(!j)return -ENOMEM;
    j->lease=*lease;j->pae_fd=pae_fd;j->rate_hz=rate_hz;j->warm_disable=warm_disable;
    j->data_fd=j->log_fd=-1;j->audio.fd=-1;
    atomic_init(&j->done,0);atomic_init(&j->phase,FH_AUDIO_RAM_NEW);
    rc=pthread_attr_init(&attr);
    if(rc){free(j);return -rc;}
    rc=pthread_attr_setstacksize(&attr,65536);
    if(!rc)rc=pthread_create(&j->thread,&attr,run,j);
    pthread_attr_destroy(&attr);
    if(rc){free(j);return -rc;}
    *out=j;return 0;
}
int fh_audio_ram_probe_start(struct fh_audio_ram_job **out,const struct fh_audio_lease *lease,int pae_fd,
                              unsigned rate_hz)
{ return fh_audio_ram_probe_start_ex(out,lease,pae_fd,rate_hz,1); }
int fh_audio_ram_probe_poll(const struct fh_audio_ram_job *j,struct fh_audio_ram_status *out)
{
    if(!j || !out)return -EINVAL;
    if(atomic_load_explicit(&j->done,memory_order_acquire)){*out=j->final;return 0;}
    memset(out,0,sizeof(*out));
    out->phase=(enum fh_audio_ram_phase)atomic_load_explicit(&j->phase,memory_order_relaxed);
    return 0;
}
