#define _GNU_SOURCE
#include "app.h"
#include "credentials.h"
#include "wifi.h"
#include "update.h"
#include "../../ota/boot_runtime.h"
#include "../../reset/config_reset.h"
#include "../../reset/button.h"
#include "../../rotation/settings.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/reboot.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
extern void camera_request_keyframe(void) __attribute__((weak));
struct control_app {
    pthread_mutex_t mutex;
    struct control_settings settings;
    struct camera_display_settings display;
    struct control_display_ops display_ops;
    int display_error;
    struct preview *preview;
    unsigned raw,lines,gain,width,height,fps,serial;
    uint64_t wb_generation;unsigned wb_red,wb_green,wb_blue,wb_cells;int wb_error;
    int stream_running,stream_result,stream_source_error;
    unsigned rotation_requested,rotation_applied,rotation_known;int rotation_error;
    struct camera_picture_settings picture;
    struct fh_encoder_settings active_encoder;
    uint64_t picture_generation,picture_applied_generation;
    int picture_error;unsigned encoder_known;
    atomic_int restart_pending;
    int directory,wifi_error;
    atomic_int update_error;
    atomic_int reset_pending,reset_button_error;
    char update_key[512],update_state[512],update_stage[512];
    unsigned boot_pending,boot_worker,boot_failed,boot_confirmed,boot_media_healthy;
    uint64_t confirmed_sequence,boot_next_retry_us;
    struct control_wifi *wifi;
    struct control_update update;
    struct fh_audio_source *audio;
    char *credentials;
    struct control_illumination_ops illumination;
    struct control_indicator_ops indicator;
};
static void *confirm_boot(void *);
#ifndef CAMERA_RAM_BOOT_TEST
static void *watch_reset_button(void *);
#endif
static int settings_valid(const struct control_settings *s)
{
    return s->brightness>=50&&s->brightness<=150&&s->shutter_ms>=10&&s->shutter_ms<=60&&
        s->shutter_ms%10==0&&(s->mains_hz==0||s->mains_hz==50||s->mains_hz==60)&&
        s->microphone<=1&&s->mic_gain<=400;
}
static void put(uint8_t *p,unsigned x)
{p[0]=x;p[1]=x>>8;p[2]=x>>16;p[3]=x>>24;}
static unsigned get(const uint8_t *p)
{return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
static int private_file(int fd)
{
    struct stat s;
    return fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)||s.st_nlink!=1?-EPERM:0;
}
static int directory(const char *path)
{
    if(mkdir(path,0700)&&errno!=EEXIST)return -errno;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -errno;
    struct stat s;
    if(fstat(fd,&s)||s.st_uid!=geteuid()||(s.st_mode&077)){close(fd);return -EPERM;}
    return fd;
}
static int load(struct control_app *a)
{
    int fd=openat(a->directory,"settings.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT?0:-errno;
    uint8_t b[29];ssize_t n=read(fd,b,sizeof b);
    int rc=private_file(fd);close(fd);
    if(rc||n!=28||memcmp(b,"CAMCFG1\0",8))return -EPROTO;
    struct control_settings s={get(b+8),get(b+12),get(b+16),get(b+20),get(b+24)};
    if(!settings_valid(&s))return -ERANGE;
    a->settings=s;return 0;
}
static int save(struct control_app *a,const struct control_settings *s)
{
    char name[64];snprintf(name,sizeof name,"settings.tmp.%ld.%u",(long)getpid(),++a->serial);
    int fd=openat(a->directory,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    uint8_t b[28];memcpy(b,"CAMCFG1\0",8);
    put(b+8,s->brightness);put(b+12,s->shutter_ms);put(b+16,s->mains_hz);put(b+20,s->microphone);put(b+24,s->mic_gain);
    int rc=0;size_t done=0;
    while(done<sizeof b){ssize_t n=write(fd,b+done,sizeof b-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(a->directory,name,a->directory,"settings.conf"))rc=-errno;
    if(!rc&&fsync(a->directory))rc=-errno;
    if(rc)unlinkat(a->directory,name,0);
    return rc;
}
static void fail(struct control_reply *r,unsigned status,const char *reason)
{r->status=status;snprintf(r->json,sizeof r->json,"{\"error\":\"%s\"}",reason);}
int control_app_open(struct control_app **out,const struct control_app_options *o,const struct control_settings *defaults)
{
    if(!out||*out||!o||!defaults||!settings_valid(defaults)||!o->credentials||!o->persistent_directory||!o->ram_directory)return -EINVAL;
#ifndef CONTROL_TESTING
    struct statfs fs;struct stat partition;
#ifdef CAMERA_RAM_BOOT_TEST
    /* Explicit SD/RAM trial build only: no JFFS2/NOR config is touched. */
    if(statfs("/etc/conf",&fs)||
       ((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)||
       lstat("/etc/conf",&partition)||partition.st_uid!=0||(partition.st_mode&022))return -EPERM;
#else
    if(statfs("/etc/conf",&fs)||(unsigned long)fs.f_type!=0x72b6UL||
       lstat("/etc/conf",&partition)||major(partition.st_dev)!=31||minor(partition.st_dev)!=5||
       partition.st_uid!=0||(partition.st_mode&022))return -EPERM;
#endif
#endif
    int dir=directory(o->persistent_directory);if(dir<0)return dir;
    int ram=directory(o->ram_directory);if(ram<0){close(dir);return ram;}
    struct statfs ramfs;
    if(fstatfs(ram,&ramfs)){close(ram);close(dir);return -errno;}
#ifndef CONTROL_TESTING
    if((unsigned long)ramfs.f_type!=0x01021994UL&&(unsigned long)ramfs.f_type!=0x858458f6UL){close(ram);close(dir);return -EPERM;}
#endif
    close(ram);
    struct control_app *a=calloc(1,sizeof *a);if(!a){close(dir);return -ENOMEM;}
    a->directory=dir;a->settings=*defaults;a->width=o->width;a->height=o->height;a->fps=o->fps;
    int rc=pthread_mutex_init(&a->mutex,NULL);
    if(rc){close(dir);free(a);return -rc;}
    rc=load(a);if(rc){close(dir);pthread_mutex_destroy(&a->mutex);free(a);return rc;}
    if(CAMERA_HAS_ROTATION&&camera_rotation_load(dir,&a->rotation_requested))fprintf(stderr,"CAMERA_ROTATION invalid settings; using normal orientation until saved\n");
    camera_picture_defaults(&a->picture);
    if(CAMERA_HAS_PICTURE&&camera_picture_load(dir,&a->picture))fprintf(stderr,"CAMERA_PICTURE invalid settings; using defaults until saved\n");
    a->picture_generation=1;atomic_init(&a->restart_pending,0);
    camera_display_defaults(&a->display);
    if(camera_display_load(dir,&a->display))fprintf(stderr,"CAMERA_DISPLAY invalid settings; using defaults until saved\n");
    a->display_error=-ENOTSUP;
    rc=camera_credentials_open(dir,o->credentials);if(rc)return rc;
    a->credentials=strdup(o->credentials);if(!a->credentials)return -ENOMEM;
    struct control_wifi_options w={o->wifi_config,o->wifi_socket,o->persistent_directory,o->dhcp_pid};
    a->wifi_error=control_wifi_init(&a->wifi,&w);
    char state[512],stage[512];
    if(snprintf(state,sizeof state,"%s/update-state",o->persistent_directory)>=(int)sizeof state||
       snprintf(stage,sizeof stage,"%s/update-stage",o->ram_directory)>=(int)sizeof stage)return -ENAMETOOLONG;
    if(!o->public_key||snprintf(a->update_key,sizeof a->update_key,"%s",o->public_key)>=(int)sizeof a->update_key)return -ENAMETOOLONG;
    strcpy(a->update_state,state);strcpy(a->update_stage,stage);
    atomic_init(&a->update_error,-EAGAIN);
    atomic_init(&a->reset_pending,0);
    atomic_init(&a->reset_button_error,-ENOTSUP);
    int needs_confirmation=camera_boot_confirmation_needed(state);
    a->boot_pending=needs_confirmation!=0;
    if(!a->boot_pending)a->update_error=control_update_init(&a->update,o->public_key,state,stage,1);
    else {
        /* Already-accepted cleanup may run before media is ready; this call
         * cannot advance a release without later capture-health samples. */
        a->boot_worker=1;pthread_t thread;
        if(pthread_create(&thread,NULL,confirm_boot,a))a->boot_worker=0;
        else pthread_detach(thread);
    }
#ifndef CAMERA_RAM_BOOT_TEST
    pthread_t reset_thread;atomic_store(&a->reset_button_error,-EAGAIN);
    int reset_rc=CAMERA_HAS_RESET?pthread_create(&reset_thread,NULL,watch_reset_button,a):0;
    if(reset_rc)atomic_store(&a->reset_button_error,-reset_rc);
    else if(CAMERA_HAS_RESET)pthread_detach(reset_thread);
#endif
    *out=a;return 0;
}
#ifndef CAMERA_RAM_BOOT_TEST
static int reset_busy(struct control_app *a)
{
    pthread_mutex_lock(&a->mutex);int busy=a->boot_worker||a->boot_pending;
    pthread_mutex_unlock(&a->mutex);
    return busy||atomic_load(&a->reset_pending)||
        (!atomic_load(&a->update_error)&&control_update_busy(&a->update));
}
static int queue_factory_reset(struct control_app *a)
{
    char error[192];int rc=-1;
    pthread_mutex_lock(&a->mutex);
    if(a->boot_worker||a->boot_pending||atomic_load(&a->reset_pending)){
        pthread_mutex_unlock(&a->mutex);return -EBUSY;
    }
    int live=!atomic_load(&a->update_error);
    if(live)pthread_mutex_lock(&a->update.mutex);
    int busy=live&&(a->update.monitor_active||a->update.install_pid>0||a->update.install_written);
    rc=camera_config_reset_request(a->directory,live?a->update.state.lock:-1,busy,NULL,NULL,error,sizeof error);
    /* A published marker after a durability error is still a reset intent.
     * Block installation until the next boot; do not reboot on an error. */
    struct stat marker;int pending=!fstatat(a->directory,CAMERA_CONFIG_RESET_MARKER,&marker,AT_SYMLINK_NOFOLLOW);
    if(!rc||pending){
        atomic_store(&a->reset_pending,1);
        if(live)a->update.reset_pending=1;
    }
    if(live)pthread_mutex_unlock(&a->update.mutex);
    pthread_mutex_unlock(&a->mutex);
    if(rc)fprintf(stderr,"CONFIG_RESET request refused: %s\n",error);
    return rc;
}
static void *watch_reset_button(void *opaque)
{
    struct control_app *a=opaque;int fd=camera_reset_button_gpio_open();
    if(fd<0){atomic_store(&a->reset_button_error,fd);fprintf(stderr,"RESET_BUTTON unavailable error=%d\n",fd);return NULL;}
    struct camera_reset_button state;camera_reset_button_init(&state);
    atomic_store(&a->reset_button_error,0);
    printf("RESET_BUTTON ready gpio=%u active_low=%s hold_seconds=%u release_to_reset=yes\n",CAMERA_RESET_PIN,CAMERA_RESET_ON_LEVEL?"no":"yes",CAMERA_RESET_HOLD_SECONDS);
    for(;;){
        struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)){close(fd);return NULL;}
        int level=camera_reset_button_gpio_read(fd);
        atomic_store(&a->reset_button_error,level<0?level:0);
        uint64_t ms=(uint64_t)now.tv_sec*1000+(unsigned long)now.tv_nsec/1000000;
        if(camera_reset_button_sample(&state,level,ms,reset_busy(a))&&!queue_factory_reset(a)){
            close(fd);puts("CONFIG_RESET queued: rebooting to clear own settings");
            struct timespec grace={.tv_sec=2};while(nanosleep(&grace,&grace)&&errno==EINTR){}
            sync();if(reboot(RB_AUTOBOOT))perror("CONFIG_RESET reboot failed; power cycle to apply queued reset");
            return NULL;
        }
        struct timespec delay={.tv_nsec=100000000};while(nanosleep(&delay,&delay)&&errno==EINTR){}
    }
}
#endif
int control_app_settings(struct control_app *a,struct control_settings *s)
{if(!a||!s)return -EINVAL;pthread_mutex_lock(&a->mutex);*s=a->settings;pthread_mutex_unlock(&a->mutex);return 0;}
void control_app_color_telemetry(struct control_app *a,uint64_t generation,unsigned red,unsigned green,unsigned blue,unsigned cells,int error)
{if(a){pthread_mutex_lock(&a->mutex);a->wb_generation=generation;a->wb_red=red;a->wb_green=green;a->wb_blue=blue;a->wb_cells=cells;a->wb_error=error;pthread_mutex_unlock(&a->mutex);}}
void control_app_stream_state(struct control_app *a,int running,int result,int source_error)
{if(a){pthread_mutex_lock(&a->mutex);a->stream_running=running!=0;a->stream_result=result;a->stream_source_error=source_error;pthread_mutex_unlock(&a->mutex);}}
int control_app_rotation_requested(struct control_app *a,unsigned *out)
{if(!a||!out)return -EINVAL;pthread_mutex_lock(&a->mutex);*out=a->rotation_requested;pthread_mutex_unlock(&a->mutex);return 0;}
void control_app_rotation_telemetry(struct control_app *a,unsigned applied,int error)
{if(a){pthread_mutex_lock(&a->mutex);a->rotation_error=error;a->rotation_known=!error;if(!error)a->rotation_applied=applied!=0;pthread_mutex_unlock(&a->mutex);}}
int control_app_picture_settings(struct control_app *a,struct camera_picture_settings *out,uint64_t *generation)
{if(!a||!out)return -EINVAL;pthread_mutex_lock(&a->mutex);*out=a->picture;if(generation)*generation=a->picture_generation;pthread_mutex_unlock(&a->mutex);return 0;}
void control_app_picture_telemetry(struct control_app *a,uint64_t generation,int error)
{if(a){pthread_mutex_lock(&a->mutex);a->picture_error=error;if(!error)a->picture_applied_generation=generation;pthread_mutex_unlock(&a->mutex);}}
void control_app_encoder_active(struct control_app *a,const struct fh_encoder_settings *s)
{if(a&&s){pthread_mutex_lock(&a->mutex);a->active_encoder=*s;a->encoder_known=1;a->width=s->width;a->height=s->height;a->fps=s->fps;pthread_mutex_unlock(&a->mutex);}}
void control_app_telemetry(struct control_app *a,unsigned raw,unsigned lines,unsigned gain)
{if(!a)return;pthread_mutex_lock(&a->mutex);a->raw=raw;a->lines=lines;a->gain=gain;pthread_mutex_unlock(&a->mutex);}
static void *confirm_boot(void *opaque)
{
    struct control_app *a=opaque;struct ota_boot_result result={0};char error[256];
    pthread_mutex_lock(&a->mutex);int healthy=a->boot_media_healthy;pthread_mutex_unlock(&a->mutex);
    int rc=-1;
    /* Retry durability cleanup before opening ordinary staging state: a prior
     * atomic accepted commit may already equal its pending sequence. */
    for(unsigned attempt=0;attempt<(healthy?3u:1u);attempt++){
        rc=camera_boot_confirm_installed(a->update_key,a->update_state,healthy,&result,error,sizeof error);
        if(rc>=0)break;
        if(attempt<2){struct timespec delay={.tv_sec=1};while(nanosleep(&delay,&delay)&&errno==EINTR){}}
    }
    if(rc<0&&!healthy){
        pthread_mutex_lock(&a->mutex);a->boot_worker=0;pthread_mutex_unlock(&a->mutex);return NULL;
    }
    int initialized=control_update_init(&a->update,a->update_key,a->update_state,a->update_stage,1);
    pthread_mutex_lock(&a->mutex);
    a->boot_failed=rc<0;a->boot_confirmed=rc==1;a->confirmed_sequence=rc==1?result.sequence:0;
    a->boot_pending=initialized!=0;a->boot_worker=0;a->update_error=initialized;
    pthread_mutex_unlock(&a->mutex);
    printf("CAMERA_BOOT_CONFIRM result=%d sequence=%llu updater_ready=%u\n",rc,(unsigned long long)(rc==1?result.sequence:0),initialized==0);fflush(stdout);
    return NULL;
}
void control_app_media_progress(struct control_app *a,unsigned video_frames,int audio_healthy)
{
    if(!a||video_frames<45)return;
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return;
    uint64_t now=(uint64_t)t.tv_sec*1000000u+(unsigned)t.tv_nsec/1000u;
    pthread_mutex_lock(&a->mutex);
    if(audio_healthy)a->boot_media_healthy=1;
    if(a->boot_pending&&!a->boot_worker&&now>=a->boot_next_retry_us){
        a->boot_worker=1;a->boot_next_retry_us=now+UINT64_C(5000000);
        pthread_t thread;int rc=pthread_create(&thread,NULL,confirm_boot,a);
        if(rc)a->boot_worker=0;else pthread_detach(thread);
    }
    pthread_mutex_unlock(&a->mutex);
}
int control_app_audio(struct control_app *a,struct fh_audio_source *source)
{if(!a||!source)return -EINVAL;a->audio=source;return fh_audio_source_controls(source,a->settings.microphone,a->settings.mic_gain);}
int control_app_illumination(struct control_app *a,const struct control_illumination_ops *ops)
{
    if(!a||!ops||!ops->apply||!ops->status_json)return -EINVAL;
    int fd=openat(a->directory,"illumination.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){if(errno!=ENOENT)return -errno;int rc=ops->apply(ops->opaque,0,0);if(!rc)a->illumination=*ops;return rc;}
    uint8_t b[17];int rc=private_file(fd);ssize_t bytes=read(fd,b,sizeof b);close(fd);
    if(rc||bytes!=16||memcmp(b,"CAMIR1\0\0",8)||get(b+8)>1||get(b+12)>1)return -EPROTO;
    rc=ops->apply(ops->opaque,get(b+8),get(b+12));
    if(!rc)a->illumination=*ops;
    return rc;
}
static int save_illumination(struct control_app *a,unsigned night,unsigned led)
{
    uint8_t b[16];memcpy(b,"CAMIR1\0\0",8);put(b+8,night);put(b+12,led);
    char name[64];snprintf(name,sizeof name,"illumination.tmp.%ld.%u",(long)getpid(),++a->serial);
    int fd=openat(a->directory,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    int rc=0;size_t done=0;
    while(done<sizeof b){ssize_t n=write(fd,b+done,sizeof b-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(a->directory,name,a->directory,"illumination.conf"))rc=-errno;
    if(!rc&&fsync(a->directory))rc=-errno;
    if(rc)unlinkat(a->directory,name,0);
    return rc;
}
int control_app_indicator(struct control_app *a,const struct control_indicator_ops *ops)
{
    if(!a||!ops||!ops->apply||!ops->status_json)return -EINVAL;
    int fd=openat(a->directory,"indicator.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){if(errno!=ENOENT)return -errno;a->indicator=*ops;return 0;}
    uint8_t b[13];int rc=private_file(fd);ssize_t bytes=read(fd,b,sizeof b);close(fd);
    if(rc||bytes!=12||memcmp(b,"CAMLED1\0",8)||get(b+8)>3)return -EPROTO;
    rc=ops->apply(ops->opaque,get(b+8));if(!rc)a->indicator=*ops;return rc;
}
static int save_indicator(struct control_app *a,unsigned mode)
{
    uint8_t b[12];memcpy(b,"CAMLED1\0",8);put(b+8,mode);
    char name[64];snprintf(name,sizeof name,"indicator.tmp.%ld.%u",(long)getpid(),++a->serial);
    int fd=openat(a->directory,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    int rc=0;size_t done=0;
    while(done<sizeof b){ssize_t n=write(fd,b+done,sizeof b-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(a->directory,name,a->directory,"indicator.conf"))rc=-errno;
    if(!rc&&fsync(a->directory))rc=-errno;
    if(rc)unlinkat(a->directory,name,0);
    return rc;
}
#ifdef CAMERA_RAM_BOOT_TEST
#define CAMERA_BOOT_MODE "ram_boot_test"
#else
#define CAMERA_BOOT_MODE "firmware_candidate"
#endif
#ifndef CAMERA_BUILD_VERSION
#define CAMERA_BUILD_VERSION "development"
#endif
void control_app_display_error(struct control_app *a,int error)
{if(a){pthread_mutex_lock(&a->mutex);a->display_error=error;pthread_mutex_unlock(&a->mutex);}}
void control_app_display_tick(struct control_app *a)
{
    if(!a)return;
    pthread_mutex_lock(&a->mutex);
    if(a->display.timestamp_enabled&&a->display_ops.apply){char text[24];int rc=camera_display_format_clock(time(NULL),a->display.timezone,text);
        if(!rc)rc=a->display_ops.apply(a->display_ops.opaque,1,text);
        a->display_error=rc;
    }
    pthread_mutex_unlock(&a->mutex);
}
int control_app_display_settings(struct control_app *a,struct camera_display_settings *out)
{
    if(!a||!out)return -EINVAL;
    pthread_mutex_lock(&a->mutex);*out=a->display;pthread_mutex_unlock(&a->mutex);return 0;
}
int control_app_display(struct control_app *a,const struct control_display_ops *ops)
{
    if(!a||!ops||!ops->apply)return -EINVAL;
    char clock[24];pthread_mutex_lock(&a->mutex);
    int rc=camera_display_format_clock(time(NULL),a->display.timezone,clock);
    if(!rc){a->display_ops=*ops;rc=ops->apply(ops->opaque,a->display.timestamp_enabled,clock);}
    a->display_error=rc;pthread_mutex_unlock(&a->mutex);return rc;
}
int control_app_preview(struct control_app *a,struct preview *preview)
{if(!a||!preview)return -EINVAL;a->preview=preview;return 0;}
static void display_status(struct control_app *a,struct control_reply *r)
{
    char name[400],clock[24],clock_json[64];pthread_mutex_lock(&a->mutex);
    int rc=control_json_string(name,sizeof name,a->display.name);
    if(camera_display_format_clock(time(NULL),a->display.timezone,clock))clock[0]=0;
    rc|=control_json_string(clock_json,sizeof clock_json,clock);
    if(!rc){r->status=200;snprintf(r->json,sizeof r->json,
        "{\"camera_name\":%s,\"timestamp_enabled\":%s,\"timezone\":\"%s\",\"camera_time\":%s,\"timestamp_available\":%s,\"timestamp_error\":%d}",
        name,a->display.timestamp_enabled?"true":"false",a->display.timezone?"UTC":"Europe/Amsterdam",clock_json,
        a->display_ops.apply?"true":"false",a->display_error);}
    pthread_mutex_unlock(&a->mutex);if(rc)fail(r,503,"display_status_failed");
}
static void status(struct control_app *a,struct control_reply *r)
{
    struct control_settings s;unsigned raw,lines,gain;
    pthread_mutex_lock(&a->mutex);s=a->settings;raw=a->raw;lines=a->lines;gain=a->gain;pthread_mutex_unlock(&a->mutex);
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    r->status=200;
    snprintf(r->json,sizeof r->json,"{\"video\":{\"width\":%u,\"height\":%u,\"fps\":%u},\"settings\":{\"brightness\":%u,\"shutter_ms\":%u,\"mains_hz\":%u,\"microphone\":%s,\"mic_gain\":%u},\"exposure\":{\"raw_q16\":%u,\"lines\":%u,\"gain_q6\":%u},\"uptime_seconds\":%llu,\"application\":\"Camera Control\",\"boot_mode\":\"" CAMERA_BOOT_MODE "\",\"firmware_version\":\"" CAMERA_BUILD_VERSION "\",\"wifi_available\":%s,\"updates_available\":%s}",a->width,a->height,a->fps,s.brightness,s.shutter_ms,s.mains_hz,s.microphone?"true":"false",s.mic_gain,raw,lines,gain,(unsigned long long)t.tv_sec,a->wifi_error?"false":"true",a->update_error?"false":"true");
    size_t n=strlen(r->json);pthread_mutex_lock(&a->mutex);
    if(n&&r->json[n-1]=='}')snprintf(r->json+n-1,sizeof r->json-n+1,",\"white_balance\":{\"generation\":%llu,\"red_q9\":%u,\"green_q9\":%u,\"blue_q9\":%u,\"reliable_cells\":%u,\"last_error\":%d}}",(unsigned long long)a->wb_generation,a->wb_red,a->wb_green,a->wb_blue,a->wb_cells,a->wb_error);
    n=strlen(r->json);
    if(n&&r->json[n-1]=='}')snprintf(r->json+n-1,sizeof r->json-n+1,",\"stream\":{\"running\":%s,\"last_error\":%d,\"source_error\":%d}}",a->stream_running?"true":"false",a->stream_result,a->stream_source_error);
    pthread_mutex_unlock(&a->mutex);
}
static void rotation_status(struct control_app *a,struct control_reply *r)
{
 pthread_mutex_lock(&a->mutex);r->status=200;
 snprintf(r->json,sizeof r->json,"{\"requested_180\":%s,\"applied_180\":%s,\"applied_known\":%s,\"pending\":%s,\"last_error\":%d}",a->rotation_requested?"true":"false",a->rotation_applied?"true":"false",a->rotation_known?"true":"false",!a->rotation_known||a->rotation_requested!=a->rotation_applied?"true":"false",a->rotation_error);
 pthread_mutex_unlock(&a->mutex);
}
static void picture_status(struct control_app *a,struct control_reply *r)
{
 char settings[4096];struct fh_encoder_plan plan;
 pthread_mutex_lock(&a->mutex);
 int rc=camera_picture_json(&a->picture,settings,sizeof settings);
 if(!rc)rc=fh_encoder_settings_plan(&a->picture.encoder,a->encoder_known?&a->active_encoder:NULL,&plan);
 if(!rc){r->status=200;snprintf(r->json,sizeof r->json,"{\"settings\":%s,\"generation\":%llu,\"live_pending\":%s,\"last_error\":%d,\"restart_required\":%s,\"restart_pending\":%s,\"capabilities\":{\"codec\":\"H264\",\"rate_control\":\"fixed_qp\",\"bitrate_control\":false,\"autofocus_control\":false,\"modes\":[\"1920x1080 at1-15fps\",\"1280x720 at30fps\"]}}",settings,(unsigned long long)a->picture_generation,a->picture_applied_generation!=a->picture_generation?"true":"false",a->picture_error,plan.restart_required?"true":"false",atomic_load(&a->restart_pending)?"true":"false");}
 pthread_mutex_unlock(&a->mutex);if(rc)fail(r,503,"picture_status_unavailable");
}
#ifndef CAMERA_RAM_BOOT_TEST
static void *restart_camera(void *opaque)
{
 struct control_app *a=opaque;struct timespec delay={.tv_sec=1};while(nanosleep(&delay,&delay)&&errno==EINTR){}
 sync();if(reboot(RB_AUTOBOOT))perror("CAMERA_RESTART failed");atomic_store(&a->restart_pending,0);return NULL;
}
#endif
static int number(const char *body,const char *key,unsigned *out)
{
    char b[32];if(control_form(body,key,b,sizeof b)!=1||!b[0])return -1;
    unsigned v=0;for(char *p=b;*p;p++){if(*p<'0'||*p>'9'||v>100000)return -1;v=v*10+(unsigned)(*p-'0');}*out=v;return 0;
}
void control_app_backend(void *opaque,const struct control_request *q,struct control_reply *r)
{
    struct control_app *a=opaque;fail(r,404,"not_found");
    if(!strcmp(q->method,"POST")&&atomic_load(&a->restart_pending)){fail(r,409,"camera_restart_pending");return;}
    if(!strcmp(q->method,"POST")&&atomic_load(&a->reset_pending)){fail(r,409,"factory_reset_pending_restart");return;}
    if(!strcmp(q->path,"/api/display")&&!strcmp(q->method,"GET")){display_status(a,r);return;}
    if(!strcmp(q->path,"/api/rotation")&&!strcmp(q->method,"GET")){rotation_status(a,r);return;}
    if(!strcmp(q->path,"/api/picture")&&!strcmp(q->method,"GET")){picture_status(a,r);return;}
    if(!strcmp(q->path,"/api/preview/status")&&!strcmp(q->method,"GET")){
        char mime[160];if(!a->preview||control_json_string(mime,sizeof mime,preview_mime(a->preview))){fail(r,503,"preview_unavailable");return;}
        r->status=200;snprintf(r->json,sizeof r->json,"{\"available\":true,\"mime\":%s,\"path\":\"/api/preview.mp4\",\"audio\":false}",mime);return;
    }
    if(!strcmp(q->path,"/api/credentials")&&!strcmp(q->method,"GET")) {
        char web[64],stream[64],wj[256],sj[256];
        if(camera_credentials_user(0,web,sizeof web)||camera_credentials_user(1,stream,sizeof stream)||control_json_string(wj,sizeof wj,web)||control_json_string(sj,sizeof sj,stream)){fail(r,503,"credentials_unavailable");return;}
        r->status=200;snprintf(r->json,sizeof r->json,"{\"web_username\":%s,\"stream_username\":%s}",wj,sj);return;
    }
    if(!strcmp(q->path,"/api/indicator")&&!strcmp(q->method,"GET")) {
        if(!a->indicator.status_json){r->status=200;snprintf(r->json,sizeof r->json,"{\"available\":false,\"mode\":\"unknown\",\"last_error\":0}");return;}
        if(a->indicator.status_json(a->indicator.opaque,r->json,sizeof r->json))fail(r,503,"indicator_status_failed");else r->status=200;
        return;
    }
    if(!strcmp(q->path,"/api/illumination")&&!strcmp(q->method,"GET")) {
        if(!a->illumination.status_json){r->status=200;snprintf(r->json,sizeof r->json,"{\"available\":false,\"filter\":\"unknown\",\"ir_led\":false,\"busy\":false}");return;}
        if(a->illumination.status_json(a->illumination.opaque,r->json,sizeof r->json))fail(r,503,"illumination_status_failed");else r->status=200;
        return;
    }
    if(!strcmp(q->method,"GET")&&(!strcmp(q->path,"/api/status")||!strcmp(q->path,"/api/settings"))){status(a,r);return;}
    if(!strcmp(q->path,"/api/update/status")&&!strcmp(q->method,"GET")) {
        if(a->update_error)fail(r,503,"update_service_unavailable");else {
            control_update_get_status(&a->update,r);
            if(r->status==200){size_t n=strlen(r->json);pthread_mutex_lock(&a->mutex);
                if(n&&r->json[n-1]=='}')snprintf(r->json+n-1,sizeof r->json-n+1,",\"boot_confirmed\":%s,\"confirmed_sequence\":%llu,\"boot_confirmation_failed\":%s}",a->boot_confirmed?"true":"false",(unsigned long long)a->confirmed_sequence,a->boot_failed?"true":"false");
                pthread_mutex_unlock(&a->mutex);
            }
        }return;
    }
    if(!strcmp(q->path,"/api/update")&&!strcmp(q->method,"POST")) {
        if(a->update_error)fail(r,503,"update_service_unavailable");else control_update_upload(&a->update,&q->body,r);return;
    }
    if((!strcmp(q->path,"/api/wifi/recovery")||!strcmp(q->path,"/api/wifi/recovery/access"))&&!strcmp(q->method,"GET")) {
        if(a->wifi_error||control_wifi_recovery_json(a->wifi,r->json,sizeof r->json,!strcmp(q->path,"/api/wifi/recovery/access")))fail(r,503,"wifi_recovery_unavailable");else r->status=200;return;
    }
    if(!strcmp(q->path,"/api/wifi/scan")&&!strcmp(q->method,"GET")) {
        if(a->wifi_error){fail(r,503,"wifi_control_unavailable");return;}
        if(control_wifi_scan_json(a->wifi,r->json,sizeof r->json))fail(r,503,"wifi_scan_results_failed");else r->status=200;return;
    }
    if(!strcmp(q->path,"/api/wifi")&&!strcmp(q->method,"GET")) {
        if(a->wifi_error){fail(r,503,"wifi_control_unavailable");return;}
        if(control_wifi_status_json(a->wifi,r->json,sizeof r->json))fail(r,503,"wifi_status_failed");else r->status=200;return;
    }
    if(strcmp(q->method,"POST"))return;
    if(!a->update_error&&control_update_busy(&a->update)){fail(r,409,"firmware_installation_in_progress");return;}
    char body[4097];if(control_body_text(&q->body,body,sizeof body)){fail(r,400,"invalid_form");return;}
    if(!strcmp(q->path,"/api/picture")){
        struct camera_picture_settings next;
        if(!strcmp(body,"action=defaults"))camera_picture_defaults(&next);
        else if(camera_picture_parse_form(body,&next)){fail(r,422,"invalid_or_unsupported_picture_settings");return;}
        pthread_mutex_lock(&a->mutex);
        if(camera_picture_save(a->directory,&next)){
            struct camera_picture_settings actual;if(!camera_picture_load(a->directory,&actual)){a->picture=actual;++a->picture_generation;}
            pthread_mutex_unlock(&a->mutex);fail(r,507,"picture_save_failed_check_settings");return;
        }
        a->picture=next;++a->picture_generation;pthread_mutex_unlock(&a->mutex);picture_status(a,r);return;
    }
    if(!strcmp(q->path,"/api/picture/keyframe")){
        if(body[0]){fail(r,400,"unexpected_form_fields");return;}
        if(!camera_request_keyframe){fail(r,503,"keyframe_control_unavailable");return;}
        camera_request_keyframe();r->status=202;strcpy(r->json,"{\"queued\":true}");return;
    }
    if(!strcmp(q->path,"/api/restart")){
        if(strcmp(body,"confirm=restart")){fail(r,400,"restart_confirmation_required");return;}
#ifdef CAMERA_RAM_BOOT_TEST
        fail(r,503,"restart_unavailable_in_ram_test");return;
#else
        if(atomic_exchange(&a->restart_pending,1)){fail(r,409,"camera_restart_pending");return;}
        pthread_t thread;int rc=pthread_create(&thread,NULL,restart_camera,a);
        if(rc){atomic_store(&a->restart_pending,0);fail(r,503,"restart_failed");return;}
        pthread_detach(thread);r->status=202;strcpy(r->json,"{\"restarting\":true}");return;
#endif
    }
    if(!strcmp(q->path,"/api/rotation")){
        char value[8];
        if(strchr(body,'&')||control_form(body,"rotate_180",value,sizeof value)!=1||(strcmp(value,"true")&&strcmp(value,"false"))){fail(r,400,"invalid_rotation");return;}
        unsigned enabled=!strcmp(value,"true");
        pthread_mutex_lock(&a->mutex);
        if(camera_rotation_save(a->directory,enabled)){
            unsigned actual;if(!camera_rotation_load(a->directory,&actual))a->rotation_requested=actual;
            pthread_mutex_unlock(&a->mutex);fail(r,507,"rotation_save_failed_check_settings");return;
        }
        a->rotation_requested=enabled;pthread_mutex_unlock(&a->mutex);rotation_status(a,r);return;
    }
    if(!strcmp(q->path,"/api/display")) {
        struct camera_display_settings next;camera_display_defaults(&next);
        char enabled[8],timezone[32];unsigned fields=1;
        for(char *p=body;*p;p++)if(*p=='&')++fields;
        if(fields!=3||control_form(body,"camera_name",next.name,sizeof next.name)!=1||
           control_form(body,"timestamp_enabled",enabled,sizeof enabled)!=1||
           control_form(body,"timezone",timezone,sizeof timezone)!=1){fail(r,400,"invalid_display_form");return;}
        if(!strcmp(enabled,"true"))next.timestamp_enabled=1;else if(strcmp(enabled,"false")){fail(r,422,"invalid_timestamp_switch");return;}
        if(!strcmp(timezone,"UTC"))next.timezone=1;else if(strcmp(timezone,"Europe/Amsterdam")){fail(r,422,"invalid_timezone");return;}
        if(!camera_display_valid(&next)){fail(r,422,"invalid_camera_name");return;}
        pthread_mutex_lock(&a->mutex);
        if(next.timestamp_enabled&&!a->display_ops.apply){pthread_mutex_unlock(&a->mutex);fail(r,503,"timestamp_unavailable");return;}
        if(camera_display_save(a->directory,&next)){pthread_mutex_unlock(&a->mutex);fail(r,507,"display_save_failed_check_settings");return;}
        a->display=next;char clock[24];int rc=0;
        if(a->display_ops.apply){rc=camera_display_format_clock(time(NULL),next.timezone,clock);if(!rc)rc=a->display_ops.apply(a->display_ops.opaque,next.timestamp_enabled,clock);a->display_error=rc;}
        pthread_mutex_unlock(&a->mutex);
        if(rc){fail(r,503,"settings_saved_but_timestamp_failed");return;}
        display_status(a,r);return;
    }
    if(!strcmp(q->path,"/api/update/plan")){
        if(body[0]){fail(r,400,"unexpected_form_fields");return;}
        if(a->update_error)fail(r,503,"update_service_unavailable");else control_update_plan(&a->update,r);
        return;
    }
    if(!strcmp(q->path,"/api/update/install")){
        char confirm[16],sequence[24],digest[129];uint64_t expected;
        if(control_form(body,"confirm",confirm,sizeof confirm)!=1||strcmp(confirm,"install")||
           control_form(body,"sequence",sequence,sizeof sequence)!=1||ota_parse_u64(sequence,&expected)||
           control_form(body,"payload_digest",digest,sizeof digest)!=1||strlen(digest)!=128){fail(r,400,"explicit_install_confirmation_required");return;}
        if(a->update_error)fail(r,503,"update_service_unavailable");else control_update_install(&a->update,expected,digest,r);
        return;
    }
    if(!strcmp(q->path,"/api/credentials")) {
        char service[16],user[64],password[128],confirm[128];
        if(control_form(body,"service",service,sizeof service)!=1||control_form(body,"username",user,sizeof user)!=1||control_form(body,"password",password,sizeof password)!=1||control_form(body,"confirm",confirm,sizeof confirm)!=1){fail(r,400,"invalid_credentials_form");return;}
        if((strcmp(service,"web")&&strcmp(service,"stream"))||strcmp(password,confirm)){fail(r,422,"invalid_credentials_or_confirmation");return;}
        int rc=camera_credentials_change(!strcmp(service,"stream"),user,password);
        memset(password,0,sizeof password);memset(confirm,0,sizeof confirm);memset(body,0,sizeof body);
        if(rc){fail(r,rc==-EINVAL?422:507,rc==-EINVAL?"invalid_username_or_password":"credentials_save_failed_check_login");return;}
        r->status=200;snprintf(r->json,sizeof r->json,"{\"saved\":true,\"service\":\"%s\"}",service);return;
    }
    if(!strcmp(q->path,"/api/indicator")) {
        char value[16];unsigned mode;
        if(strchr(body,'&')||control_form(body,"mode",value,sizeof value)!=1){fail(r,400,"invalid_indicator");return;}
        if(!strcmp(value,"off"))mode=0;else if(!strcmp(value,"red"))mode=1;else if(!strcmp(value,"green"))mode=2;else{fail(r,422,"invalid_indicator_mode");return;}
        if(!a->indicator.apply){fail(r,503,"indicator_unavailable");return;}
        int rc=a->indicator.apply(a->indicator.opaque,mode);
        if(rc){fail(r,rc==-EBUSY?409:503,"indicator_command_failed");return;}
        if(save_indicator(a,mode)){fail(r,507,"indicator_changed_but_not_saved");return;}
        if(a->indicator.status_json(a->indicator.opaque,r->json,sizeof r->json))fail(r,503,"indicator_status_failed");else r->status=200;
        return;
    }
    if(!strcmp(q->path,"/api/illumination")) {
        char filter[16],light[16];unsigned night,on,fields=1;
        for(char *p=body;*p;p++)if(*p=='&')++fields;
        if(fields!=2||control_form(body,"filter",filter,sizeof filter)!=1||control_form(body,"ir_led",light,sizeof light)!=1){fail(r,400,"invalid_illumination");return;}
        if(!strcmp(filter,"day"))night=0;else if(!strcmp(filter,"night"))night=1;else{fail(r,422,"invalid_filter_mode");return;}
        if(!strcmp(light,"true")||!strcmp(light,"1"))on=1;else if(!strcmp(light,"false")||!strcmp(light,"0"))on=0;else{fail(r,422,"invalid_ir_led");return;}
        if(!a->illumination.apply){fail(r,503,"illumination_unavailable");return;}
        int rc=a->illumination.apply(a->illumination.opaque,night,on);
        if(rc){fail(r,rc==-EBUSY||rc==-EAGAIN?409:503,rc==-EAGAIN?"filter_cooldown_try_again":"illumination_command_failed");return;}
        if(save_illumination(a,night,on)){fail(r,507,"illumination_changed_but_not_saved");return;}
        if(a->illumination.status_json(a->illumination.opaque,r->json,sizeof r->json))fail(r,503,"illumination_status_failed");else r->status=200;
        return;
    }
    if(!strcmp(q->path,"/api/update/cancel")) {
        if(body[0]){fail(r,400,"unexpected_form_fields");return;}
        if(a->update_error)fail(r,503,"update_service_unavailable");else control_update_cancel(&a->update,r);
        return;
    }
    if(!strcmp(q->path,"/api/settings")) {
        struct control_settings s;
        unsigned fields=1;for(char *p=body;*p;p++)if(*p=='&')++fields;
        char enabled[16];
        if(fields!=5||number(body,"brightness",&s.brightness)||number(body,"shutter_ms",&s.shutter_ms)||
           number(body,"mains_hz",&s.mains_hz)||number(body,"mic_gain",&s.mic_gain)||
           control_form(body,"microphone",enabled,sizeof enabled)!=1){fail(r,400,"invalid_settings");return;}
        if(!strcmp(enabled,"true")||!strcmp(enabled,"1"))s.microphone=1;
        else if(!strcmp(enabled,"false")||!strcmp(enabled,"0"))s.microphone=0;
        else {fail(r,400,"invalid_settings");return;}
        if(!settings_valid(&s)){fail(r,422,"settings_out_of_range");return;}
        if(save(a,&s)){fail(r,507,"settings_not_saved");return;}
        pthread_mutex_lock(&a->mutex);a->settings=s;pthread_mutex_unlock(&a->mutex);
        if(a->audio)fh_audio_source_controls(a->audio,s.microphone,s.mic_gain);
        status(a,r);return;
    }
    if(!strcmp(q->path,"/api/wifi/recovery")) {
        char enabled[8];if(a->wifi_error){fail(r,503,"wifi_control_unavailable");return;}
        if(control_form(body,"enabled",enabled,sizeof enabled)!=1||(strcmp(enabled,"true")&&strcmp(enabled,"false"))){fail(r,400,"invalid_recovery_request");return;}
        int rc=control_wifi_recovery_request(a->wifi,!strcmp(enabled,"true"));if(rc){fail(r,rc==-EBUSY?409:503,"wifi_recovery_command_failed");return;}
        r->status=202;strcpy(r->json,"{\"accepted\":true}");return;
    }
    if(!strcmp(q->path,"/api/wifi/scan")) {
        if(a->wifi_error){fail(r,503,"wifi_control_unavailable");return;}
        int rc=control_wifi_scan_begin(a->wifi);if(rc){fail(r,rc==-EBUSY?409:503,"wifi_scan_failed_or_busy");return;}
        r->status=202;strcpy(r->json,"{\"scanning\":true}");return;
    }
    if(!strcmp(q->path,"/api/wifi")) {
        char ssid[33],pass[65];uint64_t id=0;
        if(a->wifi_error){fail(r,503,"wifi_control_unavailable");return;}
        if(control_form(body,"ssid",ssid,sizeof ssid)!=1||control_form(body,"password",pass,sizeof pass)!=1){fail(r,400,"invalid_wifi");return;}
        int rc=control_wifi_begin(a->wifi,ssid,pass,&id);memset(pass,0,sizeof pass);memset(body,0,sizeof body);
        if(rc){fail(r,rc==-EBUSY?409:422,"wifi_change_failed");return;}
        r->status=202;snprintf(r->json,sizeof r->json,"{\"pending\":true,\"transaction\":%llu,\"confirmation_seconds\":90}",(unsigned long long)id);return;
    }
    if(!strcmp(q->path,"/api/wifi/confirm")||!strcmp(q->path,"/api/wifi/cancel")) {
        char id[32];uint64_t transaction;
        if(a->wifi_error){fail(r,503,"wifi_control_unavailable");return;}
        if(control_form(body,"transaction",id,sizeof id)!=1||ota_parse_u64(id,&transaction)){fail(r,400,"invalid_transaction");return;}
        int rc=!strcmp(q->path,"/api/wifi/confirm")?control_wifi_confirm(a->wifi,transaction):control_wifi_cancel(a->wifi,transaction);
        if(rc){fail(r,409,"wifi_transaction_failed");return;}
        r->status=200;snprintf(r->json,sizeof r->json,"{\"pending\":false,\"saved\":%s}",!strcmp(q->path,"/api/wifi/confirm")?"true":"false");return;
    }
}
static int start_preview(void *opaque,int fd)
{struct control_app *a=opaque;return a->preview?preview_stream(a->preview,fd):-ENOTSUP;}
int control_app_start(struct control_app *a,const char *bind,unsigned port)
{if(!a)return -EINVAL;struct control_http_config c={.bind_address=bind,.credentials=a->credentials,.port=port,.opaque=a,.backend=control_app_backend,.preview=start_preview};return control_http_start(&c);}
