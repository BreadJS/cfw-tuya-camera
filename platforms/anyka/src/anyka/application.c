/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "management.h"
#include "media_backend.h"
#include "audio_backend.h"
#include "picture_executor.h"
#include "wifi.h"
#include "update_http.h"
#include "application_runtime.h"
#include "reset_button.h"
#include "board_settings.h"
#include "trial_guard.h"
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef CAMERA_APPLICATION_VERSION
#define CAMERA_APPLICATION_VERSION "0.1.0-dev"
#endif
struct application {
    struct camera_media_provider video;
    struct anyka_media_backend *backend;
    struct anyka_audio *audio;
    struct anyka_management *management;
    struct anyka_executor *executor;
    struct anyka_board *board;
    struct anyka_network *network;
    struct anyka_update_http *update;
    atomic_uint frames,audio_blocks,web_ready,health_done,media_failed,last_second,reset_started,board_ready_mask;
    atomic_int board_settings_error;
    struct anyka_board_settings board_settings;
    struct anyka_video_mode video_mode;
    int video_mode_error;
    int reset_directory,app_source,application_lock;
    int calibration_attempted,calibration_error;
};
static volatile sig_atomic_t quiesce_requested,rtsp_active;
static pthread_mutex_t physical_settings_mutex=PTHREAD_MUTEX_INITIALIZER;
static void quiesce_signal(int unused)
{(void)unused;quiesce_requested=1;if(rtsp_active)(void)kill(getpid(),SIGTERM);}
static const char *environment(const char *key,const char *fallback)
{const char *v=getenv(key);return v&&v[0]?v:fallback;}
static void json_error(struct control_reply *r,unsigned status,const char *text)
{char escaped[512];control_json_string(escaped,sizeof escaped,text);r->status=status;snprintf(r->json,sizeof r->json,"{\"error\":%s}",escaped);}
static int form_boolean(const char *body,const char *key,int *value)
{char text[12];if(control_form(body,key,text,sizeof text)!=1)return -EINVAL;if(!strcmp(text,"true")||!strcmp(text,"1"))*value=1;else if(!strcmp(text,"false")||!strcmp(text,"0"))*value=0;else return -EINVAL;return 0;}
static int application_preflight(void)
{
    const char *trial=getenv("CAMERA_ANYKA_RTSP_TRIAL"),*application=getenv("CAMERA_APPLICATION");
    if(((!trial||strcmp(trial,"1"))&&(!application||strcmp(application,"1")))||geteuid()!=0)return -EPERM;
    int rc=anyka_trial_safe_sdk_host();if(rc)return rc;
    rc=anyka_trial_exclusive_owner();if(rc)return rc;
    uint32_t vi=13u*1024u*1024u;
#ifdef CAMERA_ANYKA_ENABLE_OSD
    vi+=512u*1024u;
#endif
    return anyka_trial_dma_budget(vi,10u*1024u*1024u);
}
static int application_lock_path(const char *path)
{
    int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -errno;
    struct stat st;int rc=0;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||(st.st_mode&07777)!=0600)rc=-EPERM;
    else if(flock(fd,LOCK_EX|LOCK_NB))rc=-errno;
    if(rc){close(fd);return rc;}return fd;
}
static void startup_calibration(struct application *a)
{
    if(!CAMERA_HAS_PTZ||strcmp(environment("CAMERA_PTZ_CALIBRATE","0"),"1")||a->calibration_attempted)return;
    a->calibration_attempted=1;
    printf("ANYKA_PTZ_CALIBRATION requested=yes phase=begin exclusive_owner=yes\n");fflush(stdout);
    a->calibration_error=anyka_board_calibrate(a->board);
    printf("ANYKA_PTZ_CALIBRATION phase=returned rc=%d ptz_ready=%s resources_retained=yes retry_allowed=no\n",a->calibration_error,a->calibration_error?"no":"yes");fflush(stdout);
}
static unsigned board_bit(enum anyka_board_action action)
{return action==ANYKA_BOARD_DAY?1u:action==ANYKA_BOARD_IR?2u:action==ANYKA_BOARD_LED?4u:0u;}
static int physical_apply_locked(struct application *a,enum anyka_board_action action,int value,int persist)
{
    unsigned bit=board_bit(action);if(!bit||value<0||value>1)return -EINVAL;
    if((action==ANYKA_BOARD_DAY&&!CAMERA_HAS_IRCUT)||(action==ANYKA_BOARD_IR&&!CAMERA_HAS_IR_LED)||(action==ANYKA_BOARD_LED&&!CAMERA_HAS_STATUS_LED)){if(persist)return -ENOTSUP;atomic_fetch_or(&a->board_ready_mask,bit);return 0;}
    int rc=anyka_executor_board(a->executor,action,value);
    struct anyka_board_status actual;
    if(!rc)rc=anyka_executor_status(a->executor,&actual);
    if(!rc){int observed=action==ANYKA_BOARD_DAY?!!actual.day_mode:action==ANYKA_BOARD_IR?!!actual.ir_led:!!actual.status_led;if(observed!=value)rc=-EIO;}
    if(!rc&&persist){struct anyka_board_settings next=a->board_settings;if(action==ANYKA_BOARD_DAY)next.day_mode=(unsigned)value;else if(action==ANYKA_BOARD_IR)next.ir_led=(unsigned)value;else next.status_led=(unsigned)value;
        rc=anyka_board_settings_save(a->reset_directory,&next);
        if(!rc)a->board_settings=next;else(void)anyka_board_settings_load(a->reset_directory,&a->board_settings);
    }
    if(rc){atomic_fetch_and(&a->board_ready_mask,~bit);atomic_store(&a->board_settings_error,rc);}
    else {unsigned mask=atomic_fetch_or(&a->board_ready_mask,bit)|bit;if(persist&&mask==7u)atomic_store(&a->board_settings_error,0);}
    return rc;
}
static int physical_apply(struct application *a,enum anyka_board_action action,int value)
{pthread_mutex_lock(&physical_settings_mutex);int rc=quiesce_requested?-ESHUTDOWN:physical_apply_locked(a,action,value,1);pthread_mutex_unlock(&physical_settings_mutex);return rc;}
static void startup_physical_settings(struct application *a)
{
    anyka_board_settings_defaults(&a->board_settings);
    int load=anyka_board_settings_load(a->reset_directory,&a->board_settings);
    if(load)atomic_store(&a->board_settings_error,load);
    /* Even on a corrupt saved file, establish explicit safe physical defaults;
     * keep that file/error until a user successfully saves a replacement. */
    int day=physical_apply_locked(a,ANYKA_BOARD_DAY,(int)a->board_settings.day_mode,0);
    int ir=physical_apply_locked(a,ANYKA_BOARD_IR,(int)a->board_settings.ir_led,0);
    int led=physical_apply_locked(a,ANYKA_BOARD_LED,(int)a->board_settings.status_led,0);
    int saved=0;
    if(!load&&!day&&!ir&&!led){saved=anyka_board_settings_save(a->reset_directory,&a->board_settings);atomic_store(&a->board_settings_error,saved);if(saved)atomic_store(&a->board_ready_mask,0);}
    printf("ANYKA_BOARD_SETTINGS load_rc=%d day_rc=%d IR_rc=%d LED_rc=%d save_rc=%d ready_mask=%u retained_owner=yes\n",load,day,ir,led,saved,atomic_load(&a->board_ready_mask));fflush(stdout);
}
static void physical_error(struct application *a,struct control_reply *r)
{r->status=503;snprintf(r->json,sizeof r->json,"{\"error\":\"Physical change or durable save failed; check the current state and retry\",\"partial_apply\":true,\"last_error\":%d}",atomic_load(&a->board_settings_error));}
static void board_route(struct application *a,const struct control_request *q,struct control_reply *r)
{
    struct anyka_board_status status;int rc=a->executor?anyka_executor_status(a->executor,&status):-ENODEV;
    int get=!strcmp(q->method,"GET"),post=!strcmp(q->method,"POST");
    if(!get&&!post){json_error(r,405,"Unsupported method");return;}
    if(!strcmp(q->path,"/api/illumination")) {
        if(post){char body[256],filter[16];int enabled;if(control_body_text(&q->body,body,sizeof body)||control_form(body,"filter",filter,sizeof filter)!=1||(strcmp(filter,"day")&&strcmp(filter,"night"))||form_boolean(body,"ir_led",&enabled)){json_error(r,400,"Invalid illumination values");return;}
            if(rc){json_error(r,503,"Physical controls unavailable");return;}
            rc=CAMERA_HAS_IRCUT?physical_apply(a,ANYKA_BOARD_DAY,!strcmp(filter,"day")):0;if(!rc&&CAMERA_HAS_IR_LED)rc=physical_apply(a,ANYKA_BOARD_IR,enabled);
            if(rc){physical_error(a,r);return;}rc=anyka_executor_status(a->executor,&status);}
        r->status=200;if(rc)strcpy(r->json,"{\"available\":false,\"filter\":\"unknown\"}");else snprintf(r->json,sizeof r->json,"{\"available\":true,\"busy\":false,\"filter\":\"%s\",\"ir_led\":%s,\"last_error\":%d}",status.day_mode?"day":"night",status.ir_led?"true":"false",atomic_load(&a->board_settings_error));return;
    }
    if(!strcmp(q->path,"/api/indicator")) {
        if(post){char body[128],mode[16];if(control_body_text(&q->body,body,sizeof body)||control_form(body,"mode",mode,sizeof mode)!=1||(strcmp(mode,"on")&&strcmp(mode,"off"))){json_error(r,400,"Choose On or Off");return;}
            if(rc){json_error(r,503,"Status light unavailable");return;}rc=physical_apply(a,ANYKA_BOARD_LED,!strcmp(mode,"on"));if(rc){physical_error(a,r);return;}rc=anyka_executor_status(a->executor,&status);}
        r->status=200;if(rc)strcpy(r->json,"{\"available\":false,\"mode\":\"unknown\"}");else snprintf(r->json,sizeof r->json,"{\"available\":true,\"mode\":\"%s\",\"last_error\":%d}",status.status_led?"on":"off",atomic_load(&a->board_settings_error));return;
    }
    if(!strcmp(q->path,"/api/ptz")) {
        if(a->calibration_error){if(post){json_error(r,503,"Motor calibration failed; keep this owner running and use controlled cold recovery");return;}r->status=200;snprintf(r->json,sizeof r->json,"{\"available\":false,\"calibration_failed\":true,\"last_error\":%d}",a->calibration_error);return;}
        if(post){char body[128],direction[16];if(control_body_text(&q->body,body,sizeof body)||control_form(body,"direction",direction,sizeof direction)!=1){json_error(r,400,"Invalid movement");return;}
            unsigned d=0;const char *names[]={"stop","left","right","up","down"};while(d<5&&strcmp(direction,names[d]))d++;if(d==5){json_error(r,400,"Invalid movement direction");return;}
            if(rc||!status.motors_available){json_error(r,503,"Motor controls unavailable");return;}char hold_text[12];int held=0,has_hold=control_form(body,"hold",hold_text,sizeof hold_text);
            if(has_hold<0||(has_hold==1&&form_boolean(body,"hold",&held))){json_error(r,400,"Invalid hold value");return;}
            rc=anyka_executor_board(a->executor,d?(held?ANYKA_BOARD_HOLD:ANYKA_BOARD_MOVE):ANYKA_BOARD_STOP,(int)d);if(rc){json_error(r,503,"Movement refused or failed");return;}rc=anyka_executor_status(a->executor,&status);}
        r->status=200;if(rc)strcpy(r->json,"{\"available\":false}");else snprintf(r->json,sizeof r->json,"{\"available\":%s,\"pan\":%d,\"tilt\":%d,\"moving\":%s}",status.motors_available?"true":"false",status.pan.position,status.tilt.position,status.pan.busy||status.tilt.busy?"true":"false");return;
    }
    json_error(r,404,"Unknown board route");
}
/* Persistence only: a new mode is applied by the next complete startup. */
static void video_mode_route(struct application *a,const struct control_request *q,struct control_reply *r)
{
    int post=!strcmp(q->method,"POST");
    if(!post&&strcmp(q->method,"GET")){json_error(r,405,"GET or POST required");return;}
    struct anyka_video_mode saved=a->video_mode;
    pthread_mutex_lock(&physical_settings_mutex);
    int rc=anyka_video_mode_load(a->reset_directory,&saved);
    if(post){
        char body[256],field[32];const char *names[]={"width","height","fps","profile"};unsigned values[4];
        rc=control_body_text(&q->body,body,sizeof body);
        for(unsigned i=0;!rc&&i<4;i++){
            if(control_form(body,names[i],field,sizeof field)!=1){rc=-EINVAL;break;}
            char *end=NULL;errno=0;unsigned long n=strtoul(field,&end,10);
            if(errno||!field[0]||field[0]=='-'||*end||n>65535){rc=-EINVAL;break;}values[i]=(unsigned)n;
        }
        if(!rc){saved=(struct anyka_video_mode){values[0],values[1],values[2],values[3]};rc=anyka_video_mode_validate(&saved);}
        if(!rc)rc=anyka_video_mode_save(a->reset_directory,&saved);
        if(rc){pthread_mutex_unlock(&physical_settings_mutex);json_error(r,rc==-EINVAL?400:500,"Video mode could not be saved");return;}
    }
    pthread_mutex_unlock(&physical_settings_mutex);
    r->status=200;
    snprintf(r->json,sizeof r->json,"{\"startup_only\":true,\"restart_required\":%s,\"config_error\":%d,\"startup_config_error\":%d,\"active\":{\"width\":%u,\"height\":%u,\"fps\":%u,\"profile\":%u},\"saved\":{\"width\":%u,\"height\":%u,\"fps\":%u,\"profile\":%u}}",memcmp(&saved,&a->video_mode,sizeof saved)?"true":"false",rc,a->video_mode_error,a->video_mode.width,a->video_mode.height,a->video_mode.fps,a->video_mode.profile,saved.width,saved.height,saved.fps,saved.profile);
}
static void delegate(void *opaque,const struct control_request *q,struct control_reply *r)
{
    struct application *a=opaque;
    if(!strcmp(q->path,"/api/video-mode")){video_mode_route(a,q,r);return;}
    if(!strncmp(q->path,"/api/update",11)){if(a->update)anyka_update_http_backend(a->update,q,r);else json_error(r,503,"Firmware updater unavailable");return;}
    if(!strncmp(q->path,"/api/wifi",9)){if(a->network)anyka_network_http(a->network,q,r);else json_error(r,503,"Wi-Fi manager unavailable");return;}
    board_route(a,q,r);
}
static void http_backend(void *opaque,const struct control_request *q,struct control_reply *r)
{
    struct application *a=opaque;
    if(quiesce_requested&&!strcmp(q->method,"POST")&&strncmp(q->path,"/api/update",11)){json_error(r,409,"Camera is paused for firmware installation");return;}
    anyka_management_http_backend(a->management,q,r);
}
static int http_preview(void *opaque,int fd)
{struct application *a=opaque;return quiesce_requested?-ESHUTDOWN:anyka_management_preview(a->management,fd);}
static int video(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct application *a=opaque;if(quiesce_requested)return 0;
    int rc=a->video.video(a->video.opaque,now,frame);
    if(rc==1){atomic_fetch_add(&a->frames,1);struct timespec clock;clock_gettime(CLOCK_MONOTONIC,&clock);atomic_store(&a->last_second,(unsigned)clock.tv_sec);(void)anyka_management_video(a->management,frame);}else if(rc<0){atomic_store(&a->media_failed,1);anyka_management_source_error(a->management,-EIO);}return rc;
}
static int audio(void *opaque,uint64_t now,struct camera_media_frame *frame)
{struct application *a=opaque;if(quiesce_requested)return 0;int rc=anyka_audio_read(a->audio,now,frame);if(rc==1){atomic_fetch_add(&a->audio_blocks,1);anyka_management_audio(a->management,frame);}else if(rc<0){atomic_store(&a->media_failed,1);anyka_management_source_error(a->management,-EIO);}return rc;}
static int network_ready(void)
{
    char marker[512];int length=snprintf(marker,sizeof marker,"%s/network-ready",environment("CAMERA_NETWORK_RUNTIME","/var/run/camera-network"));
    if(length<0||(size_t)length>=sizeof marker)return 0;
    int readyfd=open(marker,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(readyfd<0)return 0;
    char value[3];struct stat state;ssize_t bytes=read(readyfd,value,sizeof value);
    int verified=!fstat(readyfd,&state)&&S_ISREG(state.st_mode)&&state.st_uid==0&&!(state.st_mode&022)&&((bytes==1&&value[0]=='1')||(bytes==2&&value[0]=='1'&&value[1]=='\n'));
    close(readyfd);if(!verified)return 0;
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 0;
    struct ifreq iface={0};strcpy(iface.ifr_name,"wlan0");int ready=0;
    if(!ioctl(fd,SIOCGIFFLAGS,&iface)&&(iface.ifr_flags&IFF_UP)&&!ioctl(fd,SIOCGIFADDR,&iface)){const struct sockaddr_in *address=(const struct sockaddr_in *)&iface.ifr_addr;unsigned char first=((const unsigned char *)&address->sin_addr.s_addr)[0];ready=first&&first!=127;}
    close(fd);return ready;
}
static int web_ready_probe(void)
{
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return 0;
    struct timeval limit={0,500000};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof limit)||setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof limit)){close(fd);return 0;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(CAMERA_HTTP_PORT),.sin_addr={.s_addr=htonl(0x7f000001u)}};
    const char request[]="GET /api/status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    char response[40]={0};int ready=0;
    if(!connect(fd,(struct sockaddr *)&address,sizeof address)&&send(fd,request,sizeof request-1,MSG_NOSIGNAL)==(ssize_t)sizeof request-1){ssize_t n=recv(fd,response,sizeof response-1,0);if(n>=12&&(!memcmp(response,"HTTP/1.1 401",12)||!memcmp(response,"HTTP/1.0 401",12)))ready=1;}
    close(fd);return ready;
}
static void *health_worker(void *opaque)
{
    struct application *a=opaque;
    for(;;){if(quiesce_requested&&rtsp_active)(void)kill(getpid(),SIGTERM);
        struct timespec current;clock_gettime(CLOCK_MONOTONIC,&current);
        if(!quiesce_requested&&!a->calibration_error&&!atomic_load(&a->board_settings_error)&&atomic_load(&a->board_ready_mask)==7u&&!atomic_load(&a->media_failed)&&(unsigned)current.tv_sec-atomic_load(&a->last_second)<5&&a->update&&!atomic_load(&a->health_done)&&atomic_load(&a->frames)>=15&&(!CAMERA_HAS_AUDIO||atomic_load(&a->audio_blocks)>=5)&&atomic_load(&a->web_ready)&&network_ready()&&web_ready_probe()){
            int real_boot=a->app_source==1&&!anyka_application_ram_safe();
            if(real_boot&&!anyka_update_http_health(a->update,1,1,1,1))atomic_store(&a->health_done,1);
        }
        struct timespec pause_time={2,0};nanosleep(&pause_time,NULL);
    }
    return NULL;
}
static void retain(struct application *a,const char *stage,int rc)
{
    signal(SIGINT,SIG_IGN);signal(SIGTERM,SIG_IGN);signal(SIGHUP,SIG_IGN);
    printf("ANYKA_APPLICATION_HOLD stage=%s rc=%d resources_retained=yes hardware_dma_stopped=no\n",stage,rc);fflush(stdout);
    int handled=0;
    for(;;){if(quiesce_requested&&!handled&&a->executor){int paused=anyka_executor_pause(a->executor);anyka_management_source_error(a->management,-ESHUTDOWN);
        int proof=paused?paused:anyka_application_quiesce_receipt("/tmp/camera-update");if(!proof)handled=1;printf("ANYKA_APPLICATION_QUIESCE executor_rc=%d RAM_receipt_rc=%d DMA_resources_retained=yes\n",paused,proof);fflush(stdout);}
        struct timespec wait={0,250000000};nanosleep(&wait,NULL);}

}
extern char **environ;
static void reset_camera(void *opaque)
{
    struct application *a=opaque;
    if(quiesce_requested){printf("ANYKA_CONFIG_RESET ignored=update-quiescing\n");fflush(stdout);return;}
    if(atomic_exchange(&a->reset_started,1))return;
    int fd=openat(a->reset_directory,"reset.pending",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    int rc=fd<0?-errno:0;
    if(fd>=0){if(write(fd,"pending\n",8)!=8||fsync(fd))rc=-EIO;close(fd);if(!rc&&fsync(a->reset_directory))rc=-errno;}
    if(rc){printf("ANYKA_CONFIG_RESET refused rc=%d\n",rc);fflush(stdout);return;}
    quiesce_signal(SIGUSR1);
    pthread_mutex_lock(&physical_settings_mutex);
    if(a->executor)rc=anyka_executor_pause(a->executor);
    if(!rc)rc=anyka_reset_settings(a->reset_directory);
    pthread_mutex_unlock(&physical_settings_mutex);
    printf("ANYKA_CONFIG_RESET rc=%d OTA_state_preserved=yes rebooting=%s\n",rc,rc?"no":"yes");fflush(stdout);
    if(rc)return;
    sync();pid_t child=fork();if(child==0){char *argv[]={"reboot","-f",NULL};execve("/sbin/reboot",argv,environ);_exit(127);}
}
static int complete_pending_reset(int directory)
{
    struct stat pending;
    if(fstatat(directory,"reset.pending",&pending,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?0:-errno;
    if(!S_ISREG(pending.st_mode)||pending.st_uid!=geteuid()||pending.st_nlink!=1||(pending.st_mode&077)||pending.st_size!=8)return -EPROTO;
    int fd=openat(directory,"reset.pending",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    char text[9];ssize_t count=read(fd,text,sizeof text);close(fd);
    if(count!=8||memcmp(text,"pending\n",8))return -EPROTO;
    int rc=anyka_reset_settings(directory);printf("ANYKA_CONFIG_RESET startup_retry=yes rc=%d\n",rc);fflush(stdout);return rc;
}
int camera_anyka_application(void)
{
    /* The loader already mapped this module. Do not inject its constructor
     * into installer/reboot children, which are different executables. */
    if(unsetenv("LD_PRELOAD"))return -errno;
    static struct application a;
    signal(SIGINT,SIG_IGN);signal(SIGTERM,SIG_IGN);signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);
    struct sigaction usr={0};usr.sa_handler=quiesce_signal;sigemptyset(&usr.sa_mask);sigaction(SIGUSR1,&usr,NULL);
    atomic_init(&a.frames,0);atomic_init(&a.audio_blocks,0);atomic_init(&a.web_ready,0);atomic_init(&a.health_done,0);atomic_init(&a.media_failed,0);atomic_init(&a.last_second,0);atomic_init(&a.reset_started,0);atomic_init(&a.board_ready_mask,0);atomic_init(&a.board_settings_error,0);a.reset_directory=-1;a.application_lock=-1;
    int rc=application_preflight();if(rc)return rc;
    a.application_lock=application_lock_path("/var/run/camera-application.lock");if(a.application_lock<0)return a.application_lock;
    if(chdir("/tmp"))return -errno;
    a.app_source=anyka_application_app_source();
    int source_receipt=anyka_application_source_receipt("/tmp/camera-runtime",a.app_source);
    if(source_receipt){printf("ANYKA_APPLICATION_SOURCE receipt_rc=%d startup_refused=yes\n",source_receipt);fflush(stdout);return source_receipt;}
    printf("ANYKA_APPLICATION_SOURCE source=%s installer_enabled=%s\n",a.app_source==1?"APP-mtd7":a.app_source==2?"RAM-APP-overlay":"RAM-files-only",a.app_source==1?"yes":"no");fflush(stdout);
    int pidfd=open("/var/run/camera-application.pid",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(pidfd<0)return -errno;
    char pidtext[32];int pidbytes=snprintf(pidtext,sizeof pidtext,"%ld\n",(long)getpid());
    if(write(pidfd,pidtext,(size_t)pidbytes)!=pidbytes){close(pidfd);return -EIO;}close(pidfd);
    const char *directory=environment("CAMERA_CONFIG_DIR","/etc/config/camera");
    int config=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(config<0)return -errno;
    int reset_retry=complete_pending_reset(config);if(reset_retry){close(config);return reset_retry;}
    a.reset_directory=fcntl(config,F_DUPFD_CLOEXEC,3);if(a.reset_directory<0){int e=-errno;close(config);return e;}
    int reset_rc=anyka_reset_start(reset_camera,&a);printf("ANYKA_RESET_BUTTON rc=%d hold_seconds=5 release_to_reset=yes\n",reset_rc);fflush(stdout);
    anyka_video_mode_defaults(&a.video_mode);a.video_mode_error=anyka_video_mode_load(config,&a.video_mode);
    printf("ANYKA_VIDEO_MODE width=%u height=%u requested_fps=%u profile=%u config_error=%d fallback=%s\n",a.video_mode.width,a.video_mode.height,a.video_mode.fps,a.video_mode.profile,a.video_mode_error,a.video_mode_error?"safe-defaults":"no");fflush(stdout);
    rc=anyka_board_open(&a.board,1);if(rc){printf("ANYKA_MOTOR_UNAVAILABLE rc=%d retry_without_motors=yes\n",rc);fflush(stdout);rc=anyka_board_open(&a.board,0);}if(rc)retain(&a,"board-open",rc);
    startup_calibration(&a);
    char error[128]={0};struct camera_media_provider provider;
    rc=anyka_media_open_mode(&a.backend,&provider,&a.video_mode,error,sizeof error);
    if(rc){close(config);printf("ANYKA_APPLICATION_START_FAILED rc=%d detail=%s\n",rc,error);fflush(stdout);if(anyka_media_hardware_attempted(a.backend)||a.board)retain(&a,"media-open",rc);return rc;}
    a.video=provider;if(CAMERA_HAS_AUDIO){rc=anyka_audio_open(&a.audio);if(rc)retain(&a,"audio-open",rc);}
    provider.description.audio_codec=CAMERA_HAS_AUDIO?CAMERA_AUDIO_L16:CAMERA_AUDIO_NONE;provider.description.audio_rate=CAMERA_HAS_AUDIO?ANYKA_AUDIO_RATE:0;
    int osd=0;
#ifdef CAMERA_ANYKA_ENABLE_OSD
    osd=CAMERA_HAS_TIMESTAMP&&!strcmp(environment("CAMERA_OSD_ENABLE","0"),"1");
#endif
    struct anyka_management_options options={.version=CAMERA_APPLICATION_VERSION,.boot_mode=a.app_source==1?"app_flash":a.app_source==2?"ram_app_overlay":"ram_sdk_host",.initial_credentials="admin:camera123",.capabilities=(CAMERA_HAS_PICTURE?(ANYKA_MANAGEMENT_SCALARS|ANYKA_MANAGEMENT_ADVANCED|ANYKA_MANAGEMENT_ENCODER):0)|(CAMERA_HAS_ROTATION?ANYKA_MANAGEMENT_ROTATION:0)|(CAMERA_HAS_AUDIO?ANYKA_MANAGEMENT_AUDIO:0)|(osd?ANYKA_MANAGEMENT_TIMESTAMP:0),.width=a.video_mode.width,.height=a.video_mode.height,.fps=a.video_mode.fps,.media=provider.description,.delegate_opaque=&a,.delegate=delegate};
    rc=anyka_management_open(&a.management,config,&options);close(config);if(rc)retain(&a,"management-open",rc);
    rc=anyka_executor_open(&a.executor,a.management,a.board,environment("CAMERA_FONT_FILE",environment("CAMERA_OSD_FONT","/usr/local/ak_font_16.bin")),osd);if(rc)retain(&a,"executor-open",rc);anyka_executor_set_audio(a.executor,a.audio);
    rc=anyka_executor_set_video_fps(a.executor,a.video_mode.fps);if(rc)retain(&a,"executor-frame-limit",rc);
    int encoder_handle=anyka_media_encoder_handle(a.backend);
    rc=encoder_handle<0?encoder_handle:anyka_executor_set_encoder(a.executor,(unsigned)encoder_handle);if(rc)retain(&a,"encoder-register",rc);
    startup_physical_settings(&a);
    anyka_executor_activate(a.executor);
    char wifi_directory[512];int n=snprintf(wifi_directory,sizeof wifi_directory,"%s/wifi",directory);
    if(n<0||(size_t)n>=sizeof wifi_directory)retain(&a,"wifi-path",-ENAMETOOLONG);
    rc=anyka_network_open(&a.network,wifi_directory,environment("CAMERA_NETWORK_RUNTIME","/var/run/camera-network"));printf("ANYKA_APPLICATION_NETWORK rc=%d\n",rc);fflush(stdout);
    struct anyka_update_http_options update={.installer=environment("CAMERA_INSTALLER","/tmp/camera-runtime/camera-installer"),.runtime_directory="/tmp/camera-update",.app_root_boot=a.app_source==1};
    rc=anyka_update_http_open(&a.update,&update);printf("ANYKA_APPLICATION_UPDATE rc=%d\n",rc);fflush(stdout);
    struct control_http_config web={"0.0.0.0","admin:camera123",CAMERA_HTTP_PORT,&a,http_backend,http_preview};
    rc=control_http_start(&web);if(rc)retain(&a,"HTTP-bind",rc);atomic_store(&a.web_ready,1);
    pthread_t health;pthread_attr_t attr;int h=pthread_attr_init(&attr);if(!h){h=pthread_attr_setstacksize(&attr,128u*1024u);if(!h)h=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!h)h=pthread_create(&health,&attr,health_worker,&a);pthread_attr_destroy(&attr);}printf("ANYKA_APPLICATION_READY version=%s HTTP=81 RTSP=8554/native audio=L16-16000-mono health_thread_rc=%d ptz_calibration_error=%d\n",CAMERA_APPLICATION_VERSION,h,a.calibration_error);fflush(stdout);
    if(quiesce_requested)retain(&a,"quiesce-before-RTSP",0);
    provider.opaque=&a;provider.video=video;provider.audio=CAMERA_HAS_AUDIO?audio:NULL;
    struct camera_rtsp_config stream={"0.0.0.0",CAMERA_RTSP_PORT,"/native","admin:camera123",4,5000};
    rtsp_active=1;rc=camera_rtsp_run(&stream,&provider);rtsp_active=0;
    retain(&a,"RTSP-returned",rc);return rc;
}
#ifdef CAMERA_ANYKA_APPLICATION_CONSTRUCTOR
__attribute__((constructor)) static void constructor(void)
{int rc=camera_anyka_application();fflush(stdout);_exit(rc?125:0);}
#endif
