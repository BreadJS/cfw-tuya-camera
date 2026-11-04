#define _GNU_SOURCE
#include "experiment_owner.h"
#include "mis2006_linux_bridge.h"
#include "../capture/fh_capacity.h"
#include "../capture/fh_frame.h"
#include "../capture/fh_clock.h"
#include "../isp/isp_startup.h"
#include "../isp/isp_color_seed.h"
#include "../vmm/legacy_vmm_posix.h"
#include "../rtsp/fh_video_provider.h"
#include "../audio/ram_probe.h"
#include "../audio/source.h"
#include "../control/app.h"
#include "../illumination/controller.h"
#include "../indicator/controller.h"
#include "../isp/isp_stats.h"
#include "../isp/isp_color_auto.h"
#include "../isp/isp_awb_mode1.h"
#include "../isp/isp_shadow_runtime.h"
#include "../isp/gamma/control.h"
#include "../sensor/auto_exposure.h"
#include "../../osd/runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

/* Deliberately no normal teardown: this bounded first-frame experiment retains
 * every hardware resource for externally supervised reset. It is not a daemon,
 * production owner, profile/AE implementation or replacement firmware image. */
static struct fh_experiment_owner owner;
static int hardware_attempted;
static pthread_t event_thread;
static struct control_app *web_app;
static struct preview *web_preview;
static struct camera_osd clock_osd;
static atomic_int keyframe_requested;
void camera_request_keyframe(void){atomic_store(&keyframe_requested,1);}
static pthread_mutex_t clock_osd_mutex=PTHREAD_MUTEX_INITIALIZER;
static int osd_owner(void *opaque){return fh_experiment_owner_check(opaque);}
static int osd_submit(void *opaque,int fd,uint32_t cmd,uint32_t wire[CAMERA_GRAPHV2_WORDS])
{
    int rc=fh_experiment_owner_check(opaque);if(rc)return rc;
    if(fd<0||cmd!=CAMERA_GRAPHV2_SET||!wire)return -EINVAL;
    rc=ioctl(fd,(unsigned long)cmd,wire);return rc==-1?-errno:rc;
}
static int osd_apply(void *opaque,unsigned enabled,const char *text)
{
    struct camera_osd *osd=opaque;pthread_mutex_lock(&clock_osd_mutex);
    int rc=enabled?camera_osd_update_text(osd,text):0;
    if(!rc)rc=camera_osd_set_enabled(osd,(int)enabled);
    pthread_mutex_unlock(&clock_osd_mutex);return rc;
}
static void *clock_overlay_loop(void *opaque)
{
    (void)opaque;
    for(;;){
        control_app_display_tick(web_app);
        struct timespec delay={.tv_sec=1};while(nanosleep(&delay,&delay)&&errno==EINTR){}
    }
    return NULL;
}
static struct illumination lighting;
static struct ircut_linux ircut_backend;
static struct ir_led_linux ir_led_backend;
static struct indicator front_indicator;
static struct status_led_linux indicator_backend;
struct ae_session {
    struct fh_isp_stats stats;
    struct mis2006_runtime *sensor;
    pthread_t thread;
    uint32_t maximum_lines;
    unsigned mains_hz,line_rate;
    struct fh_linux_io *platform;
    struct fh_awb_state awb;
    int color_fault;
    struct fh_shadow_runtime shadow;
    unsigned rotate_180;
    struct camera_picture_settings picture;
    uint64_t picture_generation,picture_applied_generation,color_applied_generation,shadow_applied_generation;
    unsigned frame_max_lines,nr3d_mode,gamma_profile,gamma_enabled;
    int isp_fd,picture_fault;
    struct fh_isp_control_io isp_control;
    struct fh_capture *capture;
};
static int stats_owner(void *opaque)
{
    struct fh_linux_io *io=opaque;
    return io->check(io->owner);
}
static void update_white_balance(struct ae_session *session,const struct fh_isp_stats_sample *sample)
{
    if(!session->awb.initialized||session->color_fault)return;
    unsigned night;int lease=illumination_picture_begin(&lighting,&night);if(lease<=0)return;
    struct ir_led_status led;
    int automatic_allowed=!night&&!ir_led_get(&lighting.led,&led)&&led.commanded==0&&sample->awb_valid;
    const struct fh_awb_mode1_config config=FH_AWB_MODE1_DAY_CONFIG;
    struct fh_awb_state proposed=session->awb;
    struct fh_awb_mode1_result result={0};int rc;
    if(session->picture.auto_white_balance){
        if(automatic_allowed)rc=fh_awb_mode1_plan(&config,&proposed,&sample->awb,&result);
        else{rc=0;result.gains=proposed.gains;}
        if(rc>=0&&session->color_applied_generation!=session->picture_generation){result.gains=proposed.gains;result.changed=1;rc=1;}
    }else{
        result.gains=(struct fh_awb_gains){session->picture.wb_red_q9,session->picture.wb_green_q9,session->picture.wb_blue_q9};
        rc=fh_awb_init(&proposed,&result.gains);proposed.generation=sample->generation;
        result.changed=1;if(!rc)rc=1;
    }
    if(rc>0&&result.changed){
        uint32_t control,blc0,blc1,y0,y1;struct fh_color_plan plan;
        rc=fh_linux_io_isp_read(session->platform,0x24,&control);
        if(!rc)rc=fh_linux_io_isp_read(session->platform,0x84,&blc0);
        if(!rc)rc=fh_linux_io_isp_read(session->platform,0x88,&blc1);
        if(!rc)rc=fh_linux_io_isp_read(session->platform,0x4d8,&y0);
        if(!rc)rc=fh_linux_io_isp_read(session->platform,0x4dc,&y1);
        if(!rc)rc=fh_color_make_plan(&result.gains,(control>>1)&3,blc0,blc1,y0,y1,&plan);
        if(!rc&&session->picture.ccm_mode){
            const int identity[12]={512,0,0,0,512,0,0,0,512,0,0,0};
            const int *matrix=session->picture.ccm_mode==1?identity:session->picture.ccm;
            for(unsigned i=0;i<6;i++)plan.ccm[i]=((uint32_t)matrix[2*i]&8191u)|(((uint32_t)matrix[2*i+1]&8191u)<<16);
        }
        if(!rc)rc=fh_color_publish(session->platform,&plan,&session->color_fault);
    }else if(rc>0)rc=0;
    if(!rc){session->awb=proposed;session->color_applied_generation=session->picture_generation;}else result.gains=session->awb.gains;
    illumination_color_end(&lighting);
    if(web_app)control_app_color_telemetry(web_app,sample->generation,result.gains.red,result.gains.green,result.gains.blue,result.accepted_cells,rc);
    if(web_app&&rc<0)control_app_picture_telemetry(web_app,session->picture_generation,rc);
    if(sample->generation%20==0||rc<0)printf("NATIVE_AWB generation=%llu cells=%u logicalRGB=%u,%u,%u changed=%u error=%d\n",(unsigned long long)sample->generation,result.accepted_cells,result.gains.red,result.gains.green,result.gains.blue,result.changed,rc);
}
static void update_picture(struct ae_session *s,const struct fh_isp_stats_sample *sample)
{
 if(s->picture_fault)return;
 unsigned night;int lease=illumination_picture_begin(&lighting,&night);if(lease<=0)return;
 int rc=0;
 if(s->picture_generation!=s->picture_applied_generation){
  struct fh_picture_isp_config config=s->picture.isp;
  if(night)config.saturation=0;
  if(config.temporal_nr&&!s->nr3d_mode)rc=-ENOTSUP;
  struct fh_picture_isp_plan plan;
  if(!rc)rc=fh_picture_isp_make_plan(&config,&plan);
  if(!rc)rc=fh_picture_isp_publish(s->platform,&plan,&s->picture_fault);
  if(!rc)lighting.day_saturation=s->picture.isp.saturation;
  if(!rc&&(s->gamma_profile!=s->picture.gamma_profile||s->gamma_enabled!=s->picture.gamma_enabled)){
   rc=camera_gamma_submit_profile(s->platform,s->isp_fd,&s->isp_control,s->picture.gamma_profile,s->picture.gamma_enabled);
   if(!rc)rc=fh_linux_io_delay(s->platform,300000);
   if(!rc)rc=camera_gamma_verify_enabled(s->platform,s->picture.gamma_enabled);
   if(!rc){s->gamma_profile=s->picture.gamma_profile;s->gamma_enabled=s->picture.gamma_enabled;}
  }
  if(!rc&&!s->picture.shadow_enabled){uint32_t control;rc=fh_linux_io_isp_read(s->platform,0x24,&control);if(!rc)rc=fh_linux_io_isp_write(s->platform,0x24,control|0x40000u);}
  if(!rc)s->picture_applied_generation=s->picture_generation;
  if(rc)s->picture_fault=rc;
  if(rc)control_app_picture_telemetry(web_app,s->picture_generation,rc);
  if(!rc&&!s->picture.shadow_enabled)s->shadow_applied_generation=s->picture_generation;
 }
 if(!rc&&s->picture.shadow_enabled&&!s->shadow.fault){
  struct fh_shadow_config cfg={(uint8_t)s->picture.shadow_target,(uint8_t)s->picture.shadow_bias,(uint8_t)s->picture.shadow_low,(uint8_t)s->picture.shadow_middle,(uint8_t)s->picture.shadow_high};
  rc=fh_shadow_update_config(s->platform,&s->shadow,sample,s->sensor->gain_q6,&cfg);
  if(rc>0)s->shadow_applied_generation=s->picture_generation;
  if(rc<0)control_app_picture_telemetry(web_app,s->picture_generation,rc);
 }
 illumination_color_end(&lighting);
}
static void *exposure_loop(void *opaque)
{
    struct ae_session *session=opaque;
    struct fh_ae_config config={CAMERA_AE_TARGET_Q16,4,session->maximum_lines,64,1024,8,0,0};
    if(session->mains_hz) {
        config.shutter_quantum_num=session->line_rate;
        config.shutter_quantum_den=2U*session->mains_hz;
        config.min_lines=(config.shutter_quantum_num+config.shutter_quantum_den/2)/config.shutter_quantum_den;
    }
    int rc=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);
    if(rc){printf("NATIVE_AE_FAULT stage=cancel-policy rc=%d resources_retained=yes\n",-rc);return NULL;}
    unsigned timeouts=0;
    for(;;) {
        if(session->capture&&atomic_exchange(&keyframe_requested,0)){
            struct fh_capture *capture=session->capture;uint8_t wire[4]={0,0,0,0};
            int idr_rc=fh_experiment_owner_check(&owner);
            if(!idr_rc)idr_rc=capture->io.ioctl(capture->io.opaque,capture->pae_fd,FH_CAP_PAE_IDR,wire,sizeof wire);
            if(idr_rc)printf("NATIVE_KEYFRAME_REQUEST result=%d\n",idr_rc);
        }
        if(web_app) {
            (void)control_app_picture_settings(web_app,&session->picture,&session->picture_generation);
            unsigned requested;
            if(!control_app_rotation_requested(web_app,&requested)&&requested!=session->rotate_180){
                int rotation_rc=mis2006_set_rotation(session->sensor,requested);
                control_app_rotation_telemetry(web_app,requested,rotation_rc);
                printf("NATIVE_ROTATION requested_180=%u result=%d resources_retained=yes\n",requested,rotation_rc);
                if(rotation_rc)return NULL;
                session->rotate_180=requested;
            }
            struct control_settings live;
            if(!control_app_settings(web_app,&live)) {
                config.target_q16=CAMERA_AE_TARGET_Q16*live.brightness/100U;
                config.max_lines=(session->line_rate*live.shutter_ms+500U)/1000U;
                config.min_lines=4;config.shutter_quantum_num=0;config.shutter_quantum_den=0;
                if(live.mains_hz) {
                    config.shutter_quantum_num=session->line_rate;
                    config.shutter_quantum_den=2U*live.mains_hz;
                    config.min_lines=(session->line_rate+live.mains_hz)/(2U*live.mains_hz);
                }
            }
        }
        config.min_gain_q6=session->picture.min_gain_q6;config.max_gain_q6=session->picture.max_gain_q6;
        if(config.max_lines>session->frame_max_lines)config.max_lines=session->frame_max_lines;
        struct fh_isp_stats_sample sample;
        rc=fh_isp_stats_read(&session->stats,100,&sample);
        if(rc==-EAGAIN) {
            if(++timeouts%8==0)printf("NATIVE_AE_WAIT no_fresh_sample=%u\n",timeouts);
        } else if(rc) {
            printf("NATIVE_AE_FAULT stage=statistics rc=%d resources_retained=yes\n",rc);return NULL;
        } else {
            struct fh_ae_setting current={session->sensor->exposure,session->sensor->gain_q6},next;
            if(session->picture.auto_exposure)rc=fh_ae_plan(&config,&current,sample.raw_mean_q16,&next);
            else{
                uint32_t lines=(uint32_t)(((uint64_t)session->line_rate*session->picture.exposure_us+500000u)/1000000u);
                if(lines<4)lines=4;
                if(lines>session->frame_max_lines)lines=session->frame_max_lines;
                next=(struct fh_ae_setting){lines,session->picture.gain_q6};rc=next.lines!=current.lines||next.gain_q6!=current.gain_q6;
            }
            if(rc<0) {printf("NATIVE_AE_FAULT stage=policy rc=%d resources_retained=yes\n",rc);return NULL;}
            int changed=rc;
            if(changed) {
                rc=mis2006_set_gain(session->sensor,next.gain_q6);
                if(!rc)rc=mis2006_set_exposure(session->sensor,next.lines);
                if(rc) {printf("NATIVE_AE_FAULT stage=sensor rc=%d resources_retained=yes\n",rc);return NULL;}
            }
            if(web_app){
                update_picture(session,&sample);
                update_white_balance(session,&sample);
                if(!session->picture_fault&&!session->color_fault&&!session->shadow.fault&&
                   session->picture_applied_generation==session->picture_generation&&
                   session->color_applied_generation==session->picture_generation&&
                   session->shadow_applied_generation==session->picture_generation)
                    control_app_picture_telemetry(web_app,session->picture_generation,0);
            }
            if(web_app)control_app_telemetry(web_app,sample.raw_mean_q16,next.lines,next.gain_q6);
            printf("NATIVE_AE_SAMPLE generation=%llu irq=%u bank=%u raw_mean_q16=%u sqrt_mean_q16=%u target_raw_q16=%u sum=%llu count=%llu before_lines=%u before_gain=%u lines=%u gain_q6=%u changed=%d\n",
                (unsigned long long)sample.generation,sample.source_irq_counter,sample.bank_offset,
                sample.raw_mean_q16,sample.mean_q16,config.target_q16,(unsigned long long)sample.sum,
                (unsigned long long)sample.count,current.lines,current.gain_q6,next.lines,next.gain_q6,changed);
        }
        struct timespec delay={.tv_sec=0,.tv_nsec=250000000};
        while(nanosleep(&delay,&delay)&&errno==EINTR){}
    }
}
static int audio_owner(void *opaque,const struct fh_audio *audio,enum fh_audio_phase phase)
{
    int rc=fh_experiment_owner_check(opaque);if(rc)return rc;
    if(pthread_equal(pthread_self(),event_thread))return -EPERM;
    if(phase==FH_AUDIO_LEASE_CLOSE && audio && audio->enable_attempted)return -ENOTSUP;
    return 0;
}
static long long now_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;
    return (long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void finish(const char *stage,int rc)
{
    printf("FIRST_FRAME_RESULT stage=%s rc=%d hardware_attempted=%d\n",stage,rc,hardware_attempted);
    if(!hardware_attempted)exit(rc?1:0);
    puts("FIRST_FRAME_HOLD resources_retained=yes manual_board_reset_required=yes");
    for(;;)pause();
}
static void checked(const char *stage,int rc)
{
    printf("FIRST_FRAME_STAGE name=%s rc=%d\n",stage,rc);
    if(rc)finish(stage,rc);
}
static int device(const char *path)
{
    int rc=fh_experiment_owner_check(&owner);if(rc)finish(path,rc);
    hardware_attempted=1;
    int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0)finish(path,-errno);
    if(fstat(fd,&st)||!S_ISCHR(st.st_mode))finish(path,-ENODEV);
    return fd;
}
static int write_all(int fd,const uint8_t *data,size_t bytes)
{
    while(bytes) {
        ssize_t n=write(fd,data,bytes);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return n<0?-errno:-EIO;
        data+=n;bytes-=(size_t)n;
    }
    return 0;
}
static int encoder_always_on(void)
{
    int rc=fh_experiment_owner_check(&owner);if(rc)return rc;
    int fd=open("/proc/driver/enc",O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -errno;
    struct statfs fs;
    if(fstatfs(fd,&fs) || (unsigned long)fs.f_type!=0x9fa0UL)rc=-EPERM;
    else rc=write_all(fd,(const uint8_t *)"lowpoweroff",11);
    if(close(fd)&&!rc)rc=-errno;
    if(rc)return rc;
    /* proc write count is not an ACK: the stock handler ignores its parser
     * return. Verify that its status reader no longer reports low-power. */
    fd=open("/proc/driver/enc",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -errno;
    char text[4096];size_t used=0;
    while(used<sizeof text-1) {
        ssize_t n=read(fd,text+used,sizeof text-1-used);
        if(n<0&&errno==EINTR)continue;
        if(n<0){rc=-errno;break;}
        if(!n)break;
        used+=(size_t)n;
    }
    text[used]=0;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc && (!strstr(text,"PAE STATUS") || strstr(text,"Low Power Mode")))rc=-EPROTO;
    return rc;
}
static int credentials(const char *path,char *text,size_t capacity)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0)return -errno;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||
       (st.st_mode&077)||st.st_size<=0||st.st_size>=(off_t)capacity) {close(fd);return -EPERM;}
    ssize_t n=read(fd,text,capacity-1);int error=errno;close(fd);
    if(n!=st.st_size)return n<0?-error:-EIO;
    text[n]=0;
    while(n>0&&(text[n-1]=='\n'||text[n-1]=='\r'))text[--n]=0;
    char *colon=strchr(text,':');
    if(!colon||colon==text||!colon[1]||strchr(text,'\n')||strchr(text,'\r'))return -EINVAL;
    return 0;
}
struct rtsp_test {
    struct fh_video_provider video;
    uint64_t deadline_us,frames,bytes,first_pts,last_pts;
    size_t max_au;
    int timed_out;
    struct fh_audio_ram_job *audio_job;
    struct fh_audio_source *audio_source;
    uint64_t next_audio_report_us;
    unsigned audio_reported;
};
static int rtsp_audio(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct rtsp_test *s=opaque;
    int rc=fh_audio_source_next(s->audio_source,now,frame);
    struct fh_audio_source_status status;
    if((now>=s->next_audio_report_us||rc<0) && !fh_audio_source_status(s->audio_source,&status)) {
        s->next_audio_report_us=now+UINT64_C(1000000);
        printf("NATIVE_AUDIO_PROGRESS phase=%u error=%d blocks=%llu samples=%llu delivered=%llu dropped=%llu queued=%u warmup=%llu callback=%d\n",
            (unsigned)status.phase,status.error,(unsigned long long)status.blocks,
            (unsigned long long)status.samples,(unsigned long long)status.delivered,
            (unsigned long long)status.dropped,status.queued,
            (unsigned long long)status.warmup_blocks,rc);
    }
    return rc;
}
static void audio_report(struct rtsp_test *s)
{
    if(!s->audio_job||s->audio_reported)return;
    struct fh_audio_ram_status status;
    int rc=fh_audio_ram_probe_poll(s->audio_job,&status);
    if(!rc&&status.done) {
        s->audio_reported=1;
        printf("NATIVE_AUDIO_PROBE_DONE result=%d end=%d last_operation=%d bytes=%u written=%u blocks=%u samples=%u min=%d max=%d elapsed_ns=%llu\n",
            status.result,status.end,status.last_operation,status.bytes,status.written_bytes,
            status.blocks,status.complete_samples,status.minimum,status.maximum,(unsigned long long)status.elapsed_ns);
    }
}
static int rtsp_video(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct rtsp_test *s=opaque;
    audio_report(s);
    if(now>=s->deadline_us) {s->timed_out=1;return -1;}
    int rc=fh_video_provider_video(&s->video,now,frame);
    if(rc==1) {
        if(web_preview)(void)preview_publish(web_preview,frame->data,frame->size,frame->pts_us,frame->keyframe);
        if(!s->frames)s->first_pts=frame->pts_us;
        ++s->frames;s->bytes+=frame->size;s->last_pts=frame->pts_us;
        if(web_app){struct fh_audio_source_status audio;
            int healthy=!CAMERA_HAS_AUDIO||(s->audio_source&&!fh_audio_source_status(s->audio_source,&audio)&&
                audio.phase==FH_AUDIO_SOURCE_READ&&!audio.error&&audio.blocks>0);
            control_app_media_progress(web_app,s->frames>UINT32_MAX?UINT32_MAX:(unsigned)s->frames,healthy);
        }
        if(frame->size>s->max_au)s->max_au=frame->size;
        if(s->frames%150==0)printf("NATIVE_RTSP_PROGRESS frames=%llu bytes=%llu max_au=%zu source_pts_us=%llu\n",
            (unsigned long long)s->frames,(unsigned long long)s->bytes,s->max_au,(unsigned long long)s->last_pts);
    }
    return rc;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    event_thread=pthread_self();
    const char *seed_path=NULL;
    struct camera_picture_settings boot_picture;camera_picture_defaults(&boot_picture);
    uint32_t ae_maximum_lines=1125;
    unsigned maximum_lines_set=0;
    unsigned mains_hz=0,mains_set=0;
    unsigned experimental_fps30=0;
    unsigned output_720=0;
    unsigned encoder_clock_on=0;
    unsigned baseline_profile=0;
    const char *control_key=NULL;
    unsigned dhcp_pid=0;
    int base_argc=argc;
    while(base_argc>=4) {
        if(!strcmp(argv[base_argc-2],"--control-key")&&!control_key) {
            control_key=argv[base_argc-1];
        } else if(!strcmp(argv[base_argc-2],"--dhcp-pid")&&!dhcp_pid) {
            char *end;errno=0;unsigned long p=strtoul(argv[base_argc-1],&end,10);
            if(errno||!*argv[base_argc-1]||*end||p<2||p>4194304)return 2;
            dhcp_pid=(unsigned)p;
        } else if(!strcmp(argv[base_argc-2],"--color-seed")&&!seed_path) {
            seed_path=argv[base_argc-1];
        } else if(!strcmp(argv[base_argc-2],"--shutter-max-lines")&&!maximum_lines_set) {
            char *end;errno=0;
            const char *value=argv[base_argc-1];
            unsigned long parsed=strtoul(value,&end,10);
            if(errno||!*value||*end||parsed<4||parsed>2245)return 2;
            ae_maximum_lines=(uint32_t)parsed;maximum_lines_set=1;
        } else if(!strcmp(argv[base_argc-2],"--mains-hz")&&!mains_set) {
            if(!strcmp(argv[base_argc-1],"50"))mains_hz=50;
            else if(!strcmp(argv[base_argc-1],"60"))mains_hz=60;
            else return 2;
            mains_set=1;
        } else if(!strcmp(argv[base_argc-2],"--experimental-fps")&&!experimental_fps30 &&
                  !strcmp(argv[base_argc-1],"30")) {
            experimental_fps30=1;
        } else if(!strcmp(argv[base_argc-2],"--output-size")&&!output_720 &&
                  !strcmp(argv[base_argc-1],"1280x720")) {
            output_720=1;
        } else if(!strcmp(argv[base_argc-2],"--encoder-clock")&&!encoder_clock_on &&
                  !strcmp(argv[base_argc-1],"always-on")) {
            encoder_clock_on=1;
        } else if(!strcmp(argv[base_argc-2],"--encoder-profile")&&!baseline_profile &&
                  !strcmp(argv[base_argc-1],"baseline")) {
            baseline_profile=1;
        } else break;
        base_argc-=2;
    }
    int firmware_service=base_argc==3&&!strcmp(argv[1],"--camera-service");
    int cold_audio_live=firmware_service||(base_argc==3&&!strcmp(argv[1],"--ram-av-cold-live"));
    int cold_audio_probe=base_argc==3&&!strcmp(argv[1],"--ram-av-cold-probe");
    int temporal_bypass=base_argc==3&&(!strcmp(argv[1],"--ram-rtsp-ae-no3dnr")||cold_audio_probe||cold_audio_live);
    int auto_exposure=base_argc==3&&(!strcmp(argv[1],"--ram-rtsp-ae")||temporal_bypass);
    int audio_probe=base_argc==3&&(!strcmp(argv[1],"--ram-av-probe")||cold_audio_probe);
    int rtsp=base_argc==3&&(!strcmp(argv[1],"--ram-rtsp")||audio_probe||auto_exposure);
    uint32_t sensor_mode=experimental_fps30?MIS2006_FORMAT_204:MIS2006_FORMAT_201;
    uint32_t frame_lines=experimental_fps30?1125U:2250U;
    if(!maximum_lines_set && ae_maximum_lines>frame_lines-5)ae_maximum_lines=frame_lines-5;
    if((!rtsp&&(base_argc!=2||strcmp(argv[1],"--ram-first-frame"))) || ((maximum_lines_set||mains_set)&&!auto_exposure) ||
       (experimental_fps30&&!auto_exposure) || ((output_720||encoder_clock_on||baseline_profile)&&!experimental_fps30) || ae_maximum_lines>frame_lines-5 ||
       (control_key&&(!cold_audio_live||experimental_fps30)) || (dhcp_pid&&!control_key) ||
       (mains_hz&&ae_maximum_lines<(33750U+mains_hz)/(2U*mains_hz))) {
        fprintf(stderr,"Usage: %s --ram-first-frame | --ram-rtsp PRIVATE_AUTH_FILE | --ram-rtsp-ae PRIVATE_AUTH_FILE | --ram-rtsp-ae-no3dnr PRIVATE_AUTH_FILE | --ram-av-probe PRIVATE_AUTH_FILE | --ram-av-cold-probe PRIVATE_AUTH_FILE [--color-seed PRIVATE_SEED_FILE] [--shutter-max-lines LINES] [--mains-hz 50|60] [--experimental-fps 30]\nRequires exclusive hardware ownership and a cold boot without the vendor application. Firmware --camera-service runs continuously and never opens a watchdog.\n",argv[0]);
        return 2;
    }
    struct fh_isp_color_seed color_seed;
    if(seed_path)checked("private-color-seed",fh_isp_color_seed_read(seed_path,&color_seed));
    char auth[512]={0};
    if(rtsp)checked("private-RTSP-credentials",credentials(argv[2],auth,sizeof auth));
    if(cold_audio_live&&CAMERA_HAS_AUDIO) {
        /* Keep a plausible clock floor for logs. Initial RTCP mappings now
         * precede RTP, so VLC also plays with unsynchronized sender clocks. */
        struct timespec utc;
        checked("A-V-UTC-clock-read",clock_gettime(CLOCK_REALTIME,&utc)?-errno:0);
        checked("A-V-UTC-build-floor",utc.tv_sec<1577836800?-EAGAIN:0);
    }
    if(control_key) {
        struct control_settings defaults={100,10,mains_hz,1,100};
        struct control_app_options options={"/etc/conf/camera","/tmp/camera-control",control_key,
            "/var/tmp/camera-cold-wpa.conf","/var/run/wpa_supplicant/wlan0",auth,(pid_t)dhcp_pid,1920,1080,15};
        checked("web-control-initialization",control_app_open(&web_app,&options,&defaults));
        checked("saved-picture-settings",control_app_picture_settings(web_app,&boot_picture,NULL));
        experimental_fps30=boot_picture.encoder.fps==30;
        output_720=boot_picture.encoder.width==1280;
        sensor_mode=experimental_fps30?MIS2006_FORMAT_204:MIS2006_FORMAT_201;
        frame_lines=experimental_fps30?1125U:2250U;
        if(ae_maximum_lines>frame_lines-5)ae_maximum_lines=frame_lines-5;
    }
    struct sigaction ignore={.sa_handler=SIG_IGN};sigemptyset(&ignore.sa_mask);
    if(sigaction(SIGTERM,&ignore,NULL)||sigaction(SIGINT,&ignore,NULL)||
       sigaction(SIGHUP,&ignore,NULL)||sigaction(SIGPIPE,&ignore,NULL))return 1;
    checked("exclusive-owner",fh_experiment_owner_open(&owner));
    uint8_t *buffer=malloc(512*1024);if(!buffer)finish("application-buffer",-ENOMEM);
    int output=open("/tmp/native-first.h264",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    struct statfs fs;
    if(output<0)finish("output",-errno);
    if(fstatfs(output,&fs)||(fs.f_type!=0x858458f6 && fs.f_type!=0x01021994))finish("RAM-output",-EPERM);
    struct fh_capacity_posix capacity_posix;struct fh_capacity capacity;
    checked("capacity-backend",fh_capacity_posix_init(&capacity_posix,&owner,fh_experiment_owner_check));
    struct fh_capacity_io capacity_io={&capacity_posix,fh_capacity_posix_verify,fh_capacity_posix_write};
    checked("capacity-init",fh_capacity_init(&capacity,&capacity_io));
    hardware_attempted=1;
    checked("capacity-write",fh_capacity_prepare(&capacity));
    int media=device("/dev/media_process"),pae=device("/dev/pae"),isp_fd=device("/dev/isp");
    struct legacy_vmm_zone_lease lease={&owner,fh_experiment_owner_retain,fh_experiment_owner_release};
    struct legacy_vmm_posix vmm_posix;struct legacy_vmm vmm;struct legacy_vmm_block blocks[32];
    checked("vmm-backend",legacy_vmm_posix_init(&vmm_posix,&lease));
    checked("vmm-init",legacy_vmm_init(&vmm,legacy_vmm_posix_ops(),&vmm_posix,blocks,32));
    checked("vmm-open",legacy_vmm_open(&vmm));
    struct fh_capture_config config;fh_capture_default_config(&config);
    if(experimental_fps30) {config.fps=30;config.gop=60;config.experimental_fps30=1;}
    if(output_720) {config.output_width=1280;config.output_height=720;}
    if(baseline_profile)config.profile=66;
    if(web_app){struct fh_encoder_plan plan;checked("saved-encoder-plan",fh_encoder_settings_plan(&boot_picture.encoder,NULL,&plan));config=plan.capture;}
    printf("NATIVE_RATE_REQUEST fps=%u sensor_mode=%03x vts=%u experimental=%u observed_fps=pending\n",
        config.fps,sensor_mode,frame_lines,experimental_fps30);
    printf("NATIVE_OUTPUT_REQUEST width=%u height=%u backing_capacity=1920x1080\n",config.output_width,config.output_height);
    struct fh_capture_io capture_io={NULL,fh_capture_linux_ioctl};struct fh_capture capture;
    checked("capture-init",fh_capture_init(&capture,&config,&capture_io,&vmm,isp_fd,pae,media,LEGACY_VMM_MAP_DIRECT));
    checked("capacity-readback",fh_capture_verify_capacity(&capture,&capacity));
    checked("vpu-system",fh_capture_prepare_vpu_system(&capture));
    checked("vpu-channel-memory",fh_capture_prepare_vpu_channel_memory(&capture));
    checked("pae-system",fh_capture_prepare_encoder_system(&capture));
    if(encoder_clock_on)checked("encoder-clocks-always-on-verified",encoder_always_on());
    checked("native-vi",fh_capture_prepare_vi(&capture));
    checked("vpu-enable-ack",fh_capture_enable_vpu(&capture));
    struct fh_linux_io platform;
    checked("platform-init",fh_linux_io_init(&platform,&owner,fh_experiment_owner_check,fh_experiment_owner_quiesce));
    struct fh_isp_control_io control={NULL,fh_isp_control_linux_ioctl};struct fh_isp_startup isp;
    checked("isp-init",fh_isp_startup_init(&isp,&capture,&platform,&control));
    checked("isp-memory",fh_isp_startup_prepare_memory(&isp));
    struct mis2006_linux_owner sensor_owner={&owner,fh_experiment_owner_retain,
        fh_experiment_owner_release,fh_experiment_owner_check,fh_experiment_owner_quiesce};
    struct mis2006_linux_bridge bridge;struct mis2006_runtime sensor;
    checked("sensor-bridge",mis2006_linux_bridge_init(&bridge,&sensor,&platform,&sensor_owner));
    bridge.experimental_format204=experimental_fps30;
    checked("sensor-mode",mis2006_start(&sensor,sensor_mode));
    if(experimental_fps30) {
        uint8_t high,low;
        checked("sensor-VTS-high",mis2006_read_register(&sensor,0x3200,&high));
        checked("sensor-VTS-low",mis2006_read_register(&sensor,0x3201,&low));
        printf("NATIVE_SENSOR_VTS observed=%u expected=%u\n",((unsigned)high<<8)|low,frame_lines);
        checked("sensor-VTS-match",(((unsigned)high<<8)|low)==frame_lines?0:-EPROTO);
    }
    /* Stock observation3007=03 requires the matching odd crop origin from
     * the recovered orientation callback. The base mode table resets those
     * windows but does not reset3007, so apply both explicitly as one policy. */
    uint8_t prior_orientation;
    checked("sensor-orientation-read",mis2006_read_register(&sensor,0x3007,&prior_orientation));
    unsigned rotate_180=0;
    if(web_app)checked("saved-rotation",control_app_rotation_requested(web_app,&rotate_180));
    checked("sensor-rotation-verified",mis2006_set_rotation(&sensor,rotate_180));
    if(web_app)control_app_rotation_telemetry(web_app,rotate_180,0);
    printf("FIRST_FRAME_SENSOR_ORIENTATION before3007=%02x rotate_180=%u crop_phase=verified\n",prior_orientation,rotate_180);
    /* Legacy manual test remains explicit. AE starts from the recovered mode
     * callback's500-line/unity-gain setting, then uses fresh ISP statistics. */
    uint32_t initial_lines=auto_exposure?(mains_hz?(33750U+mains_hz)/(2U*mains_hz):(ae_maximum_lines<500?ae_maximum_lines:500)):2245;
    checked("sensor-initial-exposure",mis2006_set_exposure(&sensor,initial_lines));
    checked("sensor-initial-gain",mis2006_set_gain(&sensor,auto_exposure?64:1024));
    printf("FIRST_FRAME_EXPOSURE initial_lines=%u gain_q6=%u auto_exposure_requested=%u\n",
        sensor.exposure,sensor.gain_q6,auto_exposure?1U:0U);
    struct mis2006_stock_format format={0};
    checked("sensor-format",mis2006_stock_get_format(sensor_mode,&format));
    int isp_rc=fh_isp_startup_prepare_registers(&isp,&format);
    printf("FIRST_FRAME_ISP package_user=%08x vpu_sys_phys=%08x user=%08x size=%08x stage=%d uncertain=%d\n",
        isp.package_user,capture.vpu_sys.phys,capture.vpu_sys.user,capture.vpu_sys.size,
        isp.last_completed,isp.uncertain_stage);
    checked("isp-registers",isp_rc);
    /* Preserve the mode-derived control word. A previous diagnostic forced
     * bit23 using stock102958(0), but the observed stock mode has that bit0.
     * Do not override it with an argument absent from the actual caller. */
    uint32_t image_control;
    checked("isp-control-read",fh_linux_io_isp_read(&platform,0x24,&image_control));
    printf("FIRST_FRAME_CONTROL reg24=%08x mode_control_preserved=yes forced_bit23=no\n",image_control);
    /* Exact1044ac operation for unity RGB gains{512,512,512}. Bayer
     * permutation has no effect when all four programmed gains are equal.
     * The fixed register plan omits4bc; stock derives it from minimum gain. */
    uint32_t prior_color_scale;
    checked("isp-color-scale-read",fh_linux_io_isp_read(&platform,0x4bc,&prior_color_scale));
    checked("isp-unity-gain01",fh_linux_io_isp_write(&platform,0x224,0x02000200));
    checked("isp-unity-gain23",fh_linux_io_isp_write(&platform,0x228,0x02000200));
    checked("isp-unity-color-scale",fh_linux_io_isp_write(&platform,0x4bc,0x3ff));
    printf("FIRST_FRAME_COLOR_SCALE before=%08x after=000003ff stock_1044ac_unity=yes\n",prior_color_scale);
    if(seed_path) {
        /* Producer remains disabled until fh_isp_startup_enable below. The
         * optional seed copies observed colour words; it is not an AWB loop. */
        const uint32_t observed_offsets[]={0x84,0x88,0x4c0,0x4c4,0x4c8,0x4cc,0x4d0,0x4d4,
            0x4dc,0x4e0,0x4e4,0x4e8,0x5c8,0x5cc,0x5d0};
        for(size_t i=0;i<sizeof observed_offsets/sizeof observed_offsets[0];++i) {
            uint32_t value;
            checked("isp-color-before-read",fh_linux_io_isp_read(&platform,observed_offsets[i],&value));
            printf("FIRST_FRAME_COLOR_BEFORE offset=%03x value=%08x\n",observed_offsets[i],value);
        }
        struct fh_isp_color_seed_state seed_state={0};
        int rc=fh_isp_color_seed_apply(&color_seed,&platform,&seed_state);
        printf("FIRST_FRAME_COLOR_SEED rc=%d writes=%u prior24=%08x applied24=%08x gain01=%08x gain23=%08x clip=%08x automatic_white_balance=no\n",
            rc,seed_state.writes_completed,seed_state.previous24,seed_state.applied24,
            color_seed.gain01,color_seed.gain23,color_seed.clip);
        checked("isp-observed-color-seed",rc);
    }
    if(web_app){
        /* Accepted live colour comparison: reduce Day saturation436->320.
         * Run before illumination captures its Day value for Night restore. */
        uint32_t chroma;
        checked("day-saturation-read",fh_linux_io_isp_read(&platform,0x5d0,&chroma));
        checked("day-saturation",fh_linux_io_isp_write(&platform,0x5d0,(chroma&~UINT32_C(0x3ff))|boot_picture.isp.saturation));
    }
    if(temporal_bypass) {
        /* Exact stock102958 NR3D-off control operation. This bounded trial
         * retains the queried-mode1 backing and all DMA resources; it does
         * not change the driver mode or claim DMA quiescence. */
        uint32_t control,readback;
        checked("ISP-temporal-control-read",fh_linux_io_isp_read(&platform,0x24,&control));
        checked("ISP-temporal-bypass",fh_linux_io_isp_write(&platform,0x24,control|0x800000u));
        checked("ISP-temporal-readback",fh_linux_io_isp_read(&platform,0x24,&readback));
        checked("ISP-temporal-match",readback==(control|0x800000u)?0:-EPROTO);
        printf("NATIVE_TEMPORAL_BYPASS before24=%08x after24=%08x mode1_backing_retained=yes producer_not_started=yes\n",control,readback);
    }
    printf("FIRST_FRAME_POLICY observed_color_seed=%u native_AE_requested=%u AWB=starts_with_statistics_loop\n",
        seed_path?1U:0U,auto_exposure?1U:0U);
    if(web_app) {
        checked("IR-cut-exclusive-backend",ircut_linux_init(&ircut_backend,&owner,fh_experiment_owner_check));
        checked("IR-LED-exclusive-backend",ir_led_linux_init(&ir_led_backend,&owner,fh_experiment_owner_check));
        struct ircut_io cut_io=ircut_linux_io(&ircut_backend);
        struct ir_led_io led_io=ir_led_linux_io(&ir_led_backend);
        struct illumination_color_io color_io={&platform,fh_linux_io_isp_read,fh_linux_io_isp_write,fh_linux_io_delay};
        checked("illumination-init",illumination_init(&lighting,&cut_io,&owner,fh_experiment_owner_check,&led_io,&color_io));
        checked("illumination-safe-idle",illumination_prepare(&lighting));
        struct control_illumination_ops ops={&lighting,illumination_apply,illumination_status_json};
        checked("illumination-control",control_app_illumination(web_app,&ops));
        if(CAMERA_HAS_STATUS_LED) {
        checked("indicator-exclusive-backend",status_led_linux_init(&indicator_backend,&owner,fh_experiment_owner_check));
        struct status_led_io indicator_io=status_led_linux_io(&indicator_backend);
        /* Board color mapping is validated by the physical channel tests;
         * generic status_led drivers retain GPIO2=A / GPIO6=B terminology. */
        checked("indicator-init",indicator_init(&front_indicator,&indicator_io,STATUS_LED_CHANNEL_A,STATUS_LED_CHANNEL_B));
        checked("indicator-off-preparation",indicator_prepare_off(&front_indicator));
        struct control_indicator_ops indicator_ops={&front_indicator,indicator_apply,indicator_status_json};
        checked("indicator-control",control_app_indicator(web_app,&indicator_ops));
        }
    }
    checked("AWB-original-neutral-window",fh_color_statistics_profile(&platform));
    /* Hold generic shadow processing bypassed until the first coherent
     * statistics sample supplies a complete original profile/adaptive plan.
     * Keep gamma active and preserve temporal/bayer/illumination bits. */
    uint32_t shadow_control;
    checked("shadow-control-read",fh_linux_io_isp_read(&platform,0x24,&shadow_control));
    checked("shadow-generic-bypass",fh_linux_io_isp_write(&platform,0x24,shadow_control|UINT32_C(0x40000)));
    checked("vpu-finish",fh_capture_finish_vpu(&capture));
    checked("encoder-channel",fh_capture_prepare_encoder(&capture));
    checked("encoder-routing",fh_capture_start_routing(&capture));
    if(web_app)control_app_encoder_active(web_app,&boot_picture.encoder);
    checked("isp-producer",fh_isp_startup_enable(&isp));
    int gamma_rc=camera_gamma_submit_profile(&platform,isp_fd,&control,boot_picture.gamma_profile,boot_picture.gamma_enabled);
    if(!gamma_rc)gamma_rc=fh_linux_io_delay(&platform,300000);
    if(!gamma_rc)gamma_rc=camera_gamma_verify_enabled(&platform,boot_picture.gamma_enabled);
    printf("NATIVE_GAMMA profile=original-MIS2006 packet_bytes=652 result=%d resources_retained=yes\n",gamma_rc);
    if(web_app&&CAMERA_HAS_TIMESTAMP){
        struct camera_osd_io osd_io={&owner,osd_owner,osd_submit};
        int osd_rc=camera_osd_init(&clock_osd,isp_fd,&vmm,&osd_io,capture.config.output_width,capture.config.output_height);
        if(!osd_rc){
            struct control_display_ops display_ops={&clock_osd,osd_apply};
            osd_rc=control_app_display(web_app,&display_ops);
            if(!osd_rc){pthread_t clock_thread;int trc=pthread_create(&clock_thread,NULL,clock_overlay_loop,NULL);if(trc)osd_rc=-trc;else pthread_detach(clock_thread);}
        }
        control_app_display_error(web_app,osd_rc);
        printf("CAMERA_OSD_INIT result=%d fixed_bitmap_retained=yes\n",osd_rc);
    }
    struct fh_clock source_clock;struct fh_frame_clock frame_clock;
    struct fh_clock_io clock_io={&owner,fh_experiment_owner_check,
        fh_clock_linux_ioctl,fh_clock_monotonic_ns};
    checked("clock-init",fh_clock_init(&source_clock,pae,&clock_io));
    int clock_rc=fh_clock_calibrate(&source_clock,8);
    printf("FIRST_FRAME_SOURCE_CLOCK rc=%d rate=%u raw_reference=%llu monotonic_reference_ns=%llu window_ns=%llu\n",
        clock_rc,source_clock.rate,(unsigned long long)source_clock.raw_reference_us,
        (unsigned long long)source_clock.monotonic_reference_ns,(unsigned long long)source_clock.window_ns);
    if(!clock_rc)checked("clock-converter",fh_clock_frame_clock(&source_clock,&frame_clock));
    if(rtsp&&clock_rc)finish("RTSP-requires-validated-source-clock",clock_rc);
    if(auto_exposure) {
        struct ae_session *ae=calloc(1,sizeof *ae);
        if(!ae)finish("AE-session-memory",-ENOMEM);
        struct fh_isp_stats_io stats_io={&platform,stats_owner,fh_isp_stats_linux_ioctl,fh_linux_io_isp_read};
        checked("AE-statistics-init",fh_isp_stats_init(&ae->stats,&isp.memory,isp_fd,&stats_io));
        ae->sensor=&sensor;ae->maximum_lines=ae_maximum_lines;ae->mains_hz=mains_hz;
        ae->rotate_180=rotate_180;
        /* VPU pacing may drop output frames. Sensor line timing stays tied to
         * sensor mode201/204, independently of the configured stream rate. */
        ae->line_rate=(unsigned)format.vts*(experimental_fps30?30u:15u);
        ae->frame_max_lines=frame_lines-5;ae->nr3d_mode=isp.memory.nr3d_mode;
        ae->picture=boot_picture;ae->isp_fd=isp_fd;ae->isp_control=control;
        ae->capture=&capture;
        ae->gamma_profile=gamma_rc?10u:boot_picture.gamma_profile;ae->gamma_enabled=boot_picture.gamma_enabled;
        if(web_app)(void)control_app_picture_settings(web_app,&ae->picture,&ae->picture_generation);
        ae->platform=&platform;
        uint32_t control_word,gain01,gain23;
        int wb_rc=fh_linux_io_isp_read(&platform,0x24,&control_word);
        if(!wb_rc)wb_rc=fh_linux_io_isp_read(&platform,0x224,&gain01);
        if(!wb_rc)wb_rc=fh_linux_io_isp_read(&platform,0x228,&gain23);
        if(!wb_rc){unsigned position[4]={gain01&4095u,gain01>>16&4095u,gain23&4095u,gain23>>16&4095u};
            const uint8_t *map=camera_bayer_permutation[(control_word>>1)&3];
            struct fh_awb_gains initial={position[map[0]],position[map[1]],position[map[2]]};
            wb_rc=fh_awb_init(&ae->awb,&initial);
        }
        if(web_app&&wb_rc)control_app_color_telemetry(web_app,0,0,0,0,0,wb_rc);
        printf("NATIVE_AWB_INIT rc=%d statistics=nine_cells profile=MIS2006_day coherent_CCM=yes\n",wb_rc);
        int rc=pthread_create(&ae->thread,NULL,exposure_loop,ae);
        checked("AE-statistics-thread",rc?-rc:0);
        printf("NATIVE_AE_START target_raw_q16=11235 max_lines=%u max_gain_q6=1024 mains_hz=%u line_rate=%u AWB=fresh_nine_cell resources_retained_until_reset=yes\n",ae_maximum_lines,mains_hz,ae->line_rate);
    }
    struct fh_audio_ram_job *audio_job=NULL;
    struct fh_audio_source *audio_source=NULL;
    struct fh_clock audio_clock;
    if(cold_audio_live&&CAMERA_HAS_AUDIO) {
        struct stat audio_device;
        if(lstat("/dev/fh_audio",&audio_device)||!S_ISCHR(audio_device.st_mode))finish("audio-device-identity",-ENODEV);
        struct fh_audio_lease audio_lease={&owner,audio_device.st_rdev,audio_owner};
        /* Independent mutable error state, same immutable calibration. Real
         * cold-probe evidence confirms PCM16LE and low/high microsecond PTS. */
        audio_clock=source_clock;
        struct fh_audio_source_policy policy={1,1,1,{0}};
        checked("audio-clock-converter",fh_clock_frame_clock(&audio_clock,&policy.clock));
        checked("audio-source-thread",fh_audio_source_start(&audio_source,&audio_lease,&policy,NULL));
        if(web_app)checked("web-microphone-control",control_app_audio(web_app,audio_source));
        puts("NATIVE_AUDIO_START codec=PCMA rate=8000 channels=1 cold_boot_required=yes startup_ack=pending");
    }
    if(audio_probe) {
        struct stat audio_device;
        if(lstat("/dev/fh_audio",&audio_device)||!S_ISCHR(audio_device.st_mode))finish("audio-device-identity",-ENODEV);
        struct fh_audio_lease audio_lease={&owner,audio_device.st_rdev,audio_owner};
        checked("audio-RAM-probe-thread",fh_audio_ram_probe_start_ex(&audio_job,&audio_lease,pae,8000,cold_audio_probe?0U:1U));
        printf("NATIVE_AUDIO_PROBE_START candidate_rate=8000 channels=1 width=16 RTSP_audio=none probe_files_only=yes warm_disable=%u cold_boot_required=%u\n",
            cold_audio_probe?0U:1U,cold_audio_probe?1U:0U);
    }
    struct fh_frame_reader reader;struct fh_frame frame;
    checked("frame-reader",fh_frame_reader_init(&reader,&capture,buffer,512*1024,clock_rc?NULL:&frame_clock));
    long long start=now_ms();if(start<0)finish("monotonic-clock",-EIO);
    printf("FIRST_FRAME_CLOCK monotonic_start_ms=%lld source_units_require_comparison=yes\n",start);
    unsigned frames=0,nals=0;size_t total=0;
    while(frames<30 && total<4*1024*1024) {
        long long current=now_ms();if(current<0)finish("monotonic-clock",-EIO);
        if(current-start>=8000)break;
        int rc=fh_frame_read(&reader,&frame);
        if(rc==FH_FRAME_EMPTY) {struct timespec delay={.tv_nsec=5000000};nanosleep(&delay,NULL);continue;}
        if(rc) {
            printf("FIRST_FRAME_QUEUE acquire=%d copy=%d release=%d uncertain=%u\n",reader.last_acquire_result,
                reader.last_copy_error,reader.last_release_error,reader.ownership_uncertain);
            finish("frame-read",rc);
        }
        if(frame.bytes>4*1024*1024-total)break;
        checked("write-owned-AU",write_all(output,frame.data,frame.bytes));
        total+=frame.bytes;++frames;nals|=frame.nal_mask;
        printf("FIRST_FRAME_AU index=%u bytes=%zu raw_pts=%llu elapsed_ms=%lld sps=%u pps=%u idr=%u timestamp_ns=%llu clock_valid=%u\n",
            frames,frame.bytes,(unsigned long long)frame.raw_pts,now_ms()-start,
            frame.has_sps,frame.has_pps,frame.has_idr,(unsigned long long)frame.timestamp_ns,frame.timestamp_ns_valid);
        if(rtsp&&frame.has_sps&&frame.has_pps&&frame.has_idr) {
            checked("close-RTSP-seed-file",close(output)?-errno:0);
            struct rtsp_test session={0};struct camera_media_provider provider;
            session.audio_job=audio_job;
            session.audio_source=audio_source;
            checked("native-RTSP-provider",fh_video_provider_init(&session.video,&reader,&frame,&provider));
            unsigned duration_seconds=firmware_service?0U:audio_probe?180U:web_app?86400U:cold_audio_live?3600U:900U;
            session.deadline_us=firmware_service?UINT64_MAX:camera_monotonic_us()+(uint64_t)duration_seconds*UINT64_C(1000000);
            provider.opaque=&session;provider.video=rtsp_video;
            if(audio_source) {
                provider.description.audio_codec=CAMERA_AUDIO_PCMA;
                provider.description.audio_rate=8000;
                provider.audio=rtsp_audio;
            }
            struct camera_rtsp_config server={"0.0.0.0",CAMERA_RTSP_PORT,"/native",auth,4,1000};
            printf("NATIVE_RTSP_START bind=0.0.0.0 port=8554 path=/native auth=private-file duration_seconds=%u audio=%s\n",duration_seconds,audio_source?"PCMA/8000/mono":"none");
            if(web_app){
                struct preview_config pc={capture.config.output_width,capture.config.output_height,provider.description.sps,provider.description.pps,provider.description.sps_size,provider.description.pps_size};
                web_preview=preview_create(&pc);
                if(web_preview)control_app_preview(web_app,web_preview);
                else fprintf(stderr,"CAMERA_PREVIEW unavailable; RTSP remains active\n");
                checked("web-control-port-81",control_app_start(web_app,"0.0.0.0",CAMERA_HTTP_PORT));
            }
            if(web_app)control_app_stream_state(web_app,1,0,0);
            int result=camera_rtsp_run(&server,&provider);
            if(web_app)control_app_stream_state(web_app,0,result,session.video.last_error);
            audio_report(&session);
            printf("NATIVE_RTSP_END result=%d deadline=%d source_error=%d frames=%llu bytes=%llu max_au=%zu first_pts_us=%llu last_pts_us=%llu\n",
                result,session.timed_out,session.video.last_error,(unsigned long long)session.frames,
                (unsigned long long)session.bytes,session.max_au,(unsigned long long)session.first_pts,
                (unsigned long long)session.last_pts);
            finish("native-RTSP-test",session.timed_out?0:(result?result:-EINTR));
        }
    }
    checked("close-RAM-output",close(output)?-errno:0);
    if(isp.memory.raw.user && isp.memory.raw.bytes>=4096) {
        volatile const uint8_t *raw=(volatile const uint8_t *)(uintptr_t)isp.memory.raw.user;
        unsigned minimum=255,maximum=0,nonzero=0;unsigned long sum=0;
        for(unsigned i=0;i<4096;i++) {
            unsigned value=raw[i];if(value<minimum)minimum=value;if(value>maximum)maximum=value;
            if(value)++nonzero;
            sum+=value;
        }
        printf("FIRST_FRAME_RAW_SAMPLE bytes=4096 min=%u max=%u nonzero=%u sum=%lu not_frame_synchronized=yes\n",
            minimum,maximum,nonzero,sum);
    }
    printf("FIRST_FRAME_FILE frames=%u bytes=%zu nal_mask=%08x path=/tmp/native-first.h264\n",frames,total,nals);
    finish(rtsp?"no-RTSP-parameter-sets":frames?"copied-frames-decoder-check-required":"no-frame",
        rtsp?-ENODATA:frames?0:-ETIMEDOUT);
    return 1;
}
