#define _GNU_SOURCE
#include "audio_backend.h"
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exact target libplat_aiV1.11.02. These layouts derive from the shipped
 * getters/reader and not another SDK's similar-looking prototypes. */
struct ai_params {uint32_t rate,bits,channels,device;};
struct ai_frame {
    void *data;
    uint32_t bytes;
    uint64_t pts_ms;
    uint32_t sequence;
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct ai_params)==16,"AI open parameters are four words");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(struct ai_frame,data)==0,"AI pointer offset");
_Static_assert(offsetof(struct ai_frame,bytes)==4,"AI size offset");
_Static_assert(offsetof(struct ai_frame,pts_ms)==8,"AI clock offset");
_Static_assert(offsetof(struct ai_frame,sequence)==16,"AI sequence offset");
_Static_assert(sizeof(struct ai_frame)==32,"Conservative bounded AI descriptor");
#endif
#define AI_EMPTY 0x05000011
/* Exact mono16k start_capture DMA period:1024bytes /2 /16000 =32ms. */
#define AI_DMA_PERIOD_US 32000u
#define AI_WARMUP_MAX_BLOCKS 32u
#define AI_WARMUP_MAX_US 2000000u
struct audio_sdk {
    const char *(*version)(void);
    int (*open)(const struct ai_params *,int *);
    int (*gain)(unsigned,unsigned);
    int (*volume)(unsigned,int);
    int (*start)(unsigned);
    int (*get)(unsigned,struct ai_frame *,int);
    int (*release)(unsigned,struct ai_frame *);
    int (*actual_rate)(unsigned *);
};
struct anyka_audio {
    struct audio_sdk sdk;
    struct anyka_audio_status status;
    struct ai_frame lease;
    int handle,hardware_attempted;
    unsigned lease_owned,release_attempted,have_previous;
    uint64_t previous_ms,emitted_samples;
    unsigned actual_rate;
    uint32_t previous_bytes;
    uint64_t clock_anchor_us,clock_samples,warmup_start_us;
    atomic_uint requested_control;
    unsigned active_gain;
    uint8_t output[CAMERA_RTSP_MAX_AUDIO];
};

static int symbol(void *out,size_t size,const char *name)
{
    void *address=dlsym(RTLD_DEFAULT,name);
    if(!address||size!=sizeof address)return -ENOSYS;
    memcpy(out,&address,size);return 0;
}
/* Exact reviewed kernel capture_ioctl0x6fb8 copies runtime actual_rate+0x324
 * through ioctl0x800450ea into four bytes. Reuse the SDK-owned descriptor:
 * opening the device again would invoke capture_open and disturb ownership. */
static int capture_actual_rate(unsigned *out)
{
    struct stat device,owned;
    if(!out||lstat("/dev/akpcm_adc0",&device)||!S_ISCHR(device.st_mode))return -ENODEV;
    DIR *directory=opendir("/proc/self/fd");if(!directory)return -errno;
    struct dirent *entry;int found=-1,ambiguous=0;
    while((entry=readdir(directory))){char *end;errno=0;long number=strtol(entry->d_name,&end,10);
        if(errno||!entry->d_name[0]||*end||number<0||number>INT_MAX)continue;
        if(!fstat((int)number,&owned)&&S_ISCHR(owned.st_mode)&&owned.st_rdev==device.st_rdev){
            if(found>=0){ambiguous=1;break;}found=(int)number;
        }
    }
    closedir(directory);if(found<0||ambiguous)return -EPROTO;
    int copy=fcntl(found,F_DUPFD_CLOEXEC,3);if(copy<0)return -errno;
    int rc=0;if(fstat(copy,&owned)||!S_ISCHR(owned.st_mode)||owned.st_rdev!=device.st_rdev)rc=-EPROTO;
    unsigned rate=0;if(!rc&&ioctl(copy,0x800450eau,&rate)<0)rc=-errno;
    close(copy);if(!rc)*out=rate;return rc;
}
static int bind_sdk(struct audio_sdk *sdk)
{
    memset(sdk,0,sizeof *sdk);sdk->actual_rate=capture_actual_rate;
#define BIND(member,name) do{int rc=symbol(&sdk->member,sizeof sdk->member,name);if(rc)return rc;}while(0)
    BIND(version,"ak_ai_get_version");BIND(open,"ak_ai_open");
    BIND(gain,"ak_ai_set_gain");BIND(volume,"ak_ai_set_volume");
    BIND(start,"ak_ai_start_capture");BIND(get,"ak_ai_get_frame");
    BIND(release,"ak_ai_release_frame");
#undef BIND
    return 0;
}
static int fault(struct anyka_audio *s,int error,int sdk_result)
{
    if(!s->status.error){
        s->status.error=error<0?error:-EIO;s->status.sdk_result=sdk_result;
        fprintf(stderr,"ANYKA_AUDIO_FAULT error=%d sdk=%d candidate_ms=%llu previous_ms=%llu delta_us=%llu expected_us=%llu bytes=%u previous_bytes=%u sequence=%u previous_sequence=%u now_us=%llu clock_valid=%u phase_us=%lld warmup_discarded=%llu\n",
                s->status.error,sdk_result,(unsigned long long)s->status.candidate_source_ms,
                (unsigned long long)s->status.previous_source_ms,
                (unsigned long long)s->status.candidate_delta_us,(unsigned long long)s->status.candidate_expected_us,s->status.candidate_bytes,s->status.previous_bytes,s->status.candidate_sequence,s->status.last_sequence,
                (unsigned long long)s->status.candidate_now_us,s->status.clock_valid,(long long)s->status.candidate_phase_us,(unsigned long long)s->status.warmup_discarded);
    }
    return s->status.error;
}
static int open_bound(struct anyka_audio **out,const struct audio_sdk *sdk)
{
    if(!out||*out||!sdk||!sdk->version||!sdk->open||!sdk->gain||!sdk->volume||
       !sdk->start||!sdk->get||!sdk->release||!sdk->actual_rate)return -EINVAL;
    const char *version=sdk->version();
    if(!version||strcmp(version,"libplat_ai V1.11.02"))return -EPROTO;
    struct anyka_audio *s=calloc(1,sizeof *s);if(!s)return -ENOMEM;
    s->sdk=*sdk;s->handle=-1;*out=s;
    atomic_init(&s->requested_control,256u|5u);s->active_gain=5;
    struct ai_params params={ANYKA_AUDIO_RATE,16,ANYKA_AUDIO_CHANNELS,0};
    s->hardware_attempted=1;
    int rc=s->sdk.open(&params,&s->handle);
    if(rc)return fault(s,-EIO,rc);
    if(s->handle!=0)return fault(s,-EPROTO,0);
    /* Target open() zero-initializes AEC/NR/AGC flags. Avoid optional VQE
     * controls and preserve that baseline rather than borrowing another ABI. */
    rc=s->sdk.gain((unsigned)s->handle,5);
    if(rc)return fault(s,-EIO,rc);
    rc=s->sdk.volume((unsigned)s->handle,0);
    if(rc)return fault(s,-EIO,rc);
    rc=s->sdk.start((unsigned)s->handle);
    if(rc)return fault(s,-EIO,rc);
    rc=s->sdk.actual_rate(&s->actual_rate);
    if(rc)return fault(s,-EIO,rc);
    /* This exact16kHz mode permits only nearby divider rounding, never a
     * different codec/rate or guessed units. Measured rate also drives RTCP. */
    if(s->actual_rate<ANYKA_AUDIO_RATE*95u/100u||s->actual_rate>ANYKA_AUDIO_RATE*105u/100u)return fault(s,-EPROTO,0);
    s->status.actual_rate=s->actual_rate;
    printf("ANYKA_AUDIO_CLOCK nominal=%u actual=%u source=owned-ADC-readonly-ioctl\n",ANYKA_AUDIO_RATE,s->actual_rate);fflush(stdout);
    s->status.started=1;
    return 0;
}
int anyka_audio_open(struct anyka_audio **out)
{
    if(!out||*out)return -EINVAL;
    struct audio_sdk sdk;int rc=bind_sdk(&sdk);if(rc)return rc;
    return open_bound(out,&sdk);
}
int anyka_audio_control(struct anyka_audio *s,unsigned enabled,unsigned percent)
{
    if(!s||enabled>1||percent>100)return -EINVAL;
    atomic_store(&s->requested_control,(enabled?256u:0u)|((percent*8u+50u)/100u));
    return 0;
}
static int release_lease(struct anyka_audio *s)
{
    if(!s->lease_owned||s->release_attempted)return fault(s,-ENOTRECOVERABLE,0);
    s->release_attempted=1;
    int rc=s->sdk.release((unsigned)s->handle,&s->lease);
    if(rc)return fault(s,-EIO,rc);
    /* Exact release frees and zeros pointer/length/PTS/sequence. A mismatch
     * cannot safely be retried: the buffer may already have been freed. */
    if(s->lease.data||s->lease.bytes||s->lease.pts_ms||s->lease.sequence)
        return fault(s,-EPROTO,0);
    s->lease_owned=0;s->release_attempted=0;
    memset(&s->lease,0,sizeof s->lease);
    return 0;
}
int anyka_audio_read(void *opaque,uint64_t now_us,struct camera_media_frame *out)
{
    struct anyka_audio *s=opaque;
    if(out)memset(out,0,sizeof *out);
    if(!s||!out)return -1;
    if(s->status.error||!s->status.started)return -1;
    if(s->lease_owned||s->lease.data){fault(s,-ENOTRECOVERABLE,0);return -1;}
    unsigned control=atomic_load(&s->requested_control);
    if((control&255u)!=s->active_gain){
        int gain_rc=s->sdk.gain((unsigned)s->handle,control&255u);
        if(gain_rc){fault(s,-EIO,gain_rc);return -1;}
        s->active_gain=control&255u;
    }
    memset(&s->lease,0,sizeof s->lease);
    int rc=s->sdk.get((unsigned)s->handle,&s->lease,0);
    if(rc==AI_EMPTY){
        if(s->lease.data){fault(s,-ENOTRECOVERABLE,rc);return -1;}
        return 0;
    }
    if(rc){fault(s,s->lease.data?-ENOTRECOVERABLE:-EIO,rc);return -1;}
    if(!s->lease.data){fault(s,-EPROTO,0);return -1;}
    s->lease_owned=1;s->release_attempted=0;
    uint32_t bytes=s->lease.bytes,sequence=s->lease.sequence;
    uint64_t raw=s->lease.pts_ms;
    int error=0;
    uint64_t gap=0;int retry_clock=0;
    s->status.candidate_source_ms=raw;s->status.previous_source_ms=s->previous_ms;
    s->status.candidate_sequence=sequence;s->status.candidate_now_us=now_us;
    s->status.candidate_phase_us=0;
    s->status.candidate_delta_us=0;s->status.candidate_bytes=bytes;s->status.previous_bytes=s->previous_bytes;
    s->status.candidate_expected_us=(uint64_t)s->previous_bytes*1000000u/(2u*s->actual_rate);
    if(!bytes||bytes>sizeof s->output||(bytes&1u))error=-EMSGSIZE;
    if(!raw||raw>UINT64_MAX/1000u)error=-ERANGE;
    uint64_t stamp=raw<=UINT64_MAX/1000u?raw*1000u:0;
    uint64_t distance=stamp>now_us?stamp-now_us:now_us-stamp;
    if(distance>5000000u)error=-ERANGE;
    if(s->have_previous){
        if(raw<=s->previous_ms)error=-ERANGE;
        else if(raw<=UINT64_MAX/1000u){
            uint64_t delta=(raw-s->previous_ms)*1000u;
            s->status.candidate_delta_us=delta;
            uint64_t expected=(uint64_t)s->previous_bytes*1000000u/(2u*s->actual_rate);
            uint64_t difference=delta>expected?delta-expected:expected-delta;
            if(!s->status.clock_valid){
                if(difference>2000u){
                    /* Discard bounded initial IRQ jitter without claiming
                     * units. Only a later duration-consistent pair proves ms. */
                    if(difference<AI_DMA_PERIOD_US&&s->status.warmup_discarded<AI_WARMUP_MAX_BLOCKS&&now_us>=s->warmup_start_us&&now_us-s->warmup_start_us<=AI_WARMUP_MAX_US)retry_clock=1;
                    else error=-ERANGE;
                }
            }else{
                if(delta>1000000u)error=-ERANGE;
                uint64_t elapsed=(s->clock_samples/s->actual_rate)*1000000u+
                    (s->clock_samples%s->actual_rate)*1000000u/s->actual_rate;
                if(UINT64_MAX-s->clock_anchor_us<elapsed)error=-ERANGE;
                uint64_t grid=s->clock_anchor_us+elapsed;
                uint64_t phase=stamp>grid?stamp-grid:grid-stamp;
                uint64_t period=(512000000u+s->actual_rate-1u)/s->actual_rate;
                s->status.candidate_phase_us=phase>INT64_MAX?INT64_MAX:stamp>=grid?(int64_t)phase:-(int64_t)phase;
                /* The driver interpolates the period's monotonic IRQ timestamp,
                 * not a sample counter. Keep a source-anchored sample lattice
                 * only while phase stays strictly below one DMA period.
                 * A whole-period forward step remains a real source gap, allowing
                 * 2ms for the SDK timestamp rounding; backward phase/drift
                 * beyond one period plus that rounding budget is rejected. */
                if(stamp>=grid&&phase>=period-2000u)gap=phase;
                else if(stamp<grid&&phase>=period+2000u)error=-ERANGE;
                else {
                    /* A slow bounded phase servo follows fractional divider /
                     * oscillator error; a permanently fixed integer-rate anchor
                     * would drift over hours. One IRQ pulse changes phase by at
                     * most period/64, while inconsistent clocks still hit the
                     * one-period guard. RTP itself uses exact sample positions. */
                    int64_t adjust=s->status.candidate_phase_us/64;
                    int64_t limit=(int64_t)(expected/16u);if(limit<1)limit=1;
                    if(adjust>limit)adjust=limit;
                    if(adjust< -limit)adjust= -limit;
                    if(adjust<0){uint64_t magnitude=(uint64_t)(-adjust);if(s->clock_anchor_us<magnitude)error=-ERANGE;else{s->clock_anchor_us-=magnitude;stamp=grid-magnitude;}}
                    else if(UINT64_MAX-s->clock_anchor_us<(uint64_t)adjust||UINT64_MAX-grid<(uint64_t)adjust)error=-ERANGE;
                    else{s->clock_anchor_us+=(uint64_t)adjust;stamp=grid+(uint64_t)adjust;}
                }
            }
        }
    }
    if(!error){
        const uint8_t *input=s->lease.data;
        for(uint32_t i=0;i<bytes;i+=2){s->output[i]=input[i+1];s->output[i+1]=input[i];}
    }
    rc=release_lease(s);
    if(rc)return -1;
    if(error){fault(s,error,0);return -1;}
    if(gap){
        s->status.discontinuities++;s->status.last_gap_us=gap;
        uint64_t missing=(gap*s->actual_rate+500000u)/1000000u;
        if(UINT64_MAX-s->emitted_samples<missing){fault(s,-ERANGE,0);return -1;}
        s->emitted_samples+=missing;
    }
    s->status.blocks++;s->status.samples+=bytes/2u;
    s->status.last_sequence=sequence;s->status.last_source_ms=raw;
    int priming=!s->have_previous||retry_clock;
    if(!s->have_previous)s->warmup_start_us=now_us;
    s->have_previous=1;s->previous_ms=raw;s->previous_bytes=bytes;
    if(priming){s->status.warmup_discarded++;return 0;}
    if(!s->status.clock_valid||gap){s->clock_anchor_us=stamp;s->clock_samples=bytes/2u;}
    else{s->clock_samples+=bytes/2u;if(s->status.candidate_phase_us)s->status.jittered_blocks++;}
    if(!(control&256u))memset(s->output,0,bytes);
    s->status.clock_valid=1;
    if(UINT64_MAX-s->emitted_samples<bytes/2u){fault(s,-ERANGE,0);return -1;}
    *out=(struct camera_media_frame){.data=s->output,.size=bytes,.pts_us=stamp,.sample_position=s->emitted_samples,.sample_clock_rate=s->actual_rate};
    s->emitted_samples+=bytes/2u;s->status.sample_position=s->emitted_samples;
    if((s->status.blocks&1023u)==0){printf("ANYKA_AUDIO_PROGRESS blocks=%llu actual_rate=%u samples=%llu jittered=%llu gaps=%llu source_ms=%llu\n",(unsigned long long)s->status.blocks,s->actual_rate,(unsigned long long)s->emitted_samples,(unsigned long long)s->status.jittered_blocks,(unsigned long long)s->status.discontinuities,(unsigned long long)raw);fflush(stdout);}
    return 1;
}
int anyka_audio_status(const struct anyka_audio *s,struct anyka_audio_status *out)
{
    if(!s||!out)return -EINVAL;
    *out=s->status;return 0;
}
