/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "management.h"
#include "../native/control/credentials.h"
#include "../preview/preview.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct anyka_management {
    pthread_mutex_t lock;
    int directory;
    struct anyka_management_settings desired;
    struct camera_display_settings display;
    unsigned revision,completed,inflight;
    int last_error,encoder_ready;
    struct anyka_encoder_snapshot encoder_state;
    struct anyka_management_options options;
    char version[64],boot_mode[64];
    struct preview *preview;
    atomic_uint frames,audio_blocks,last_frame_second,preview_drops;
    atomic_int source_error;
};
static const char *keys[6]={"hue","brightness_offset","saturation_offset","contrast_offset","sharpness_offset","wdr_offset"};
struct advanced_field {const char *name;size_t offset;};
#define FIELD(name) {#name,offsetof(struct anyka_advanced_settings,name)}
static const struct advanced_field advanced_fields[]={FIELD(auto_exposure),FIELD(max_exposure_us),FIELD(min_exposure_us),FIELD(exposure_us),FIELD(gain_q8),FIELD(min_gain_q8),FIELD(max_gain_q8),FIELD(auto_white_balance),FIELD(wb_red_q8),FIELD(wb_green_q8),FIELD(wb_blue_q8),FIELD(temporal_nr),FIELD(gamma_lift)};
#undef FIELD
_Static_assert(sizeof(struct anyka_advanced_settings)==56,"persisted14int32");
static const struct advanced_field encoder_fields[]={
 {"encoder_target_kbps",offsetof(struct anyka_encoder_settings,target_kbps)},
 {"encoder_maximum_kbps",offsetof(struct anyka_encoder_settings,maximum_kbps)},
 {"encoder_gop",offsetof(struct anyka_encoder_settings,gop_frames)},
 {"encoder_qp",offsetof(struct anyka_encoder_settings,initial_qp)},
 {"encoder_min_qp",offsetof(struct anyka_encoder_settings,minimum_qp)},
 {"encoder_max_qp",offsetof(struct anyka_encoder_settings,maximum_qp)},
 {"encoder_rate_control",offsetof(struct anyka_encoder_settings,rate_control)}};
_Static_assert(sizeof(struct anyka_encoder_settings)==28,"persisted7u32");
static int encoder_valid(const struct anyka_encoder_settings *s)
{return s->target_kbps>=1&&s->target_kbps<=65535&&s->maximum_kbps>=s->target_kbps&&s->maximum_kbps<=65535&&s->gop_frames>=1&&s->gop_frames<=1000&&s->minimum_qp<=s->initial_qp&&s->initial_qp<=s->maximum_qp&&s->maximum_qp<=51&&s->rate_control<=1;}
extern void camera_request_keyframe(void) __attribute__((weak));
static unsigned now_seconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(unsigned)t.tv_sec;}
static void error_reply(struct control_reply *r,unsigned status,const char *message)
{char escaped[1024];r->status=status;if(control_json_string(escaped,sizeof escaped,message)<0)strcpy(escaped,"\"Request failed\"");snprintf(r->json,sizeof r->json,"{\"error\":%s}",escaped);}
static int valid(const struct anyka_management_settings *s)
{for(unsigned i=0;i<6;i++)if(s->effects[i]<-50||s->effects[i]>50)return 0;return(s->mains_hz==50||s->mains_hz==60)&&s->rotate_180<=1&&s->microphone<=1&&s->mic_gain<=100&&anyka_advanced_valid(&s->advanced)&&s->encoder_configured<=1&&encoder_valid(&s->encoder);}
static int private_file(int fd,off_t expected)
{struct stat s;return fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&07777)!=0600||s.st_nlink!=1||s.st_size!=expected;}
static uint32_t get32(const unsigned char *p)
{return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,uint32_t v)
{p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static int load_settings(int dir,struct anyka_management_settings *s)
{
    memset(s,0,sizeof *s);s->mains_hz=50;s->microphone=1;s->mic_gain=100;anyka_advanced_defaults(&s->advanced);s->encoder=(struct anyka_encoder_settings){512,1024,30,35,25,45,1};
    int fd=openat(dir,"anyka-picture.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return errno==ENOENT?0:-errno;
    unsigned char data[136];ssize_t n=read(fd,data,sizeof data);int bad=private_file(fd,n);close(fd);
    int old=n==48&&!memcmp(data,"AKPIC001",8),current=n==104&&!memcmp(data,"AKPIC002",8),v3=n==136&&!memcmp(data,"AKPIC003",8);
    if(bad||(!old&&!current&&!v3))return -EPROTO;
    for(unsigned i=0;i<6;i++)s->effects[i]=(int32_t)get32(data+8+i*4);
    s->mains_hz=get32(data+32);s->rotate_180=get32(data+36);s->microphone=get32(data+40);s->mic_gain=get32(data+44);
    if(current||v3)for(unsigned i=0;i<14;i++){int value=(int32_t)get32(data+48+i*4);memcpy((unsigned char *)&s->advanced+i*4,&value,4);}
    if(v3){s->encoder_configured=get32(data+104);for(unsigned i=0;i<7;i++){unsigned value=get32(data+108+i*4);memcpy((unsigned char *)&s->encoder+i*4,&value,4);}}
    return valid(s)?0:-EPROTO;
}
static int save_settings(int dir,const struct anyka_management_settings *s)
{
    unsigned char data[136]={0};memcpy(data,"AKPIC003",8);
    for(unsigned i=0;i<6;i++)put32(data+8+i*4,(uint32_t)s->effects[i]);
    put32(data+32,s->mains_hz);put32(data+36,s->rotate_180);put32(data+40,s->microphone);put32(data+44,s->mic_gain);
    for(unsigned i=0;i<14;i++){int value;memcpy(&value,(const unsigned char *)&s->advanced+i*4,4);put32(data+48+i*4,(uint32_t)value);}
    put32(data+104,s->encoder_configured);for(unsigned i=0;i<7;i++){unsigned value;memcpy(&value,(const unsigned char *)&s->encoder+i*4,4);put32(data+108+i*4,value);}
    int fd=openat(dir,"anyka-picture.tmp",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    int rc=0;size_t done=0;
    while(done<sizeof data){ssize_t n=write(fd,data+done,sizeof data-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(dir,"anyka-picture.tmp",dir,"anyka-picture.conf"))rc=-errno;
    if(!rc&&fsync(dir))rc=-errno;
    if(rc)unlinkat(dir,"anyka-picture.tmp",0);
    return rc;
}
static int integer(const char *body,const char *key,int *value)
{char text[40],*end=NULL;int rc=control_form(body,key,text,sizeof text);if(rc<=0)return rc;if(!text[0])return -1;errno=0;long n=strtol(text,&end,10);if(errno||*end||n<-2147483647L||n>2147483647L)return -1;*value=(int)n;return 1;}
static int boolean(const char *body,const char *key,unsigned *out)
{char text[12];int rc=control_form(body,key,text,sizeof text);if(rc<=0)return rc;if(!strcmp(text,"true")||!strcmp(text,"1"))*out=1;else if(!strcmp(text,"false")||!strcmp(text,"0"))*out=0;else return -1;return 1;}
static int pending(const struct anyka_management *m)
{return m->revision!=m->completed;}
static void queue(struct anyka_management *m)
{if(++m->revision==0)m->revision=1;m->last_error=0;}
static void picture_reply(struct anyka_management *m,struct control_reply *r)
{
    const struct anyka_management_settings *s=&m->desired;
    r->status=200;
    int used=snprintf(r->json,sizeof r->json,"{\"platform\":\"anyka\",\"max_exposure_us\":%u,\"available\":%s,\"advanced_available\":%s,\"configured\":%s,\"encoder_available\":%s,\"encoder_configured\":%s,\"settings\":{",anyka_advanced_max_us(m->options.fps),m->options.capabilities&ANYKA_MANAGEMENT_SCALARS?"true":"false",m->options.capabilities&ANYKA_MANAGEMENT_ADVANCED?"true":"false",s->advanced.configured?"true":"false",m->encoder_ready?"true":"false",s->encoder_configured?"true":"false");
    for(unsigned i=0;i<6;i++)used+=snprintf(r->json+used,sizeof r->json-(size_t)used,"\"%s\":%d,",keys[i],s->effects[i]);
    for(unsigned i=0;i<sizeof advanced_fields/sizeof advanced_fields[0];i++){int value;memcpy(&value,(const unsigned char *)&s->advanced+advanced_fields[i].offset,sizeof value);used+=snprintf(r->json+used,sizeof r->json-(size_t)used,"\"%s\":%d,",advanced_fields[i].name,value);}
    for(unsigned i=0;i<sizeof encoder_fields/sizeof encoder_fields[0];i++){unsigned value;memcpy(&value,(const unsigned char *)&s->encoder+encoder_fields[i].offset,4);used+=snprintf(r->json+used,sizeof r->json-(size_t)used,"\"%s\":%u,",encoder_fields[i].name,value);}
    snprintf(r->json+used,sizeof r->json-(size_t)used,"\"mains_hz\":%u},\"live_pending\":%s,\"last_error\":%d,\"restart_required\":false,\"encoder\":{\"error\":%d,\"sdk_result\":%d,\"target_kbps\":%u,\"maximum_kbps\":%u,\"gop_frames\":%u,\"initial_qp\":%u,\"minimum_qp\":%u,\"maximum_qp\":%u,\"rate_control\":%u,\"idr_requests\":%lu,\"iframe_overrides_modified\":false}}",s->mains_hz,pending(m)?"true":"false",m->last_error,m->encoder_state.error,m->encoder_state.sdk_result,m->encoder_state.settings.target_kbps,m->encoder_state.settings.maximum_kbps,m->encoder_state.settings.gop_frames,m->encoder_state.settings.initial_qp,m->encoder_state.settings.minimum_qp,m->encoder_state.settings.maximum_qp,m->encoder_state.settings.rate_control,m->encoder_state.idr_requests);
}
static void rotation_reply(struct anyka_management *m,struct control_reply *r)
{r->status=200;snprintf(r->json,sizeof r->json,"{\"available\":%s,\"requested_180\":%s,\"pending\":%s,\"last_error\":%d}",m->options.capabilities&ANYKA_MANAGEMENT_ROTATION?"true":"false",m->desired.rotate_180?"true":"false",pending(m)?"true":"false",m->last_error);}
static void display_reply(struct anyka_management *m,struct control_reply *r)
{
    char name[400],clock[24]={0};time_t now=time(NULL);
    control_json_string(name,sizeof name,m->display.name);camera_display_format_clock(now,m->display.timezone,clock);
    r->status=200;snprintf(r->json,sizeof r->json,"{\"camera_name\":%s,\"timestamp_enabled\":%s,\"timestamp_available\":%s,\"timezone\":\"%s\",\"camera_time\":\"%s\",\"timestamp_error\":%d,\"pending\":%s}",name,m->display.timestamp_enabled?"true":"false",m->options.capabilities&ANYKA_MANAGEMENT_TIMESTAMP?"true":"false",m->display.timezone?"UTC":"Europe/Amsterdam",clock,m->last_error,pending(m)?"true":"false");
}
static void status_reply(struct anyka_management *m,struct control_reply *r)
{
    unsigned last=atomic_load(&m->last_frame_second);int source_error=atomic_load(&m->source_error);
    char version[400],mode[400];control_json_string(version,sizeof version,m->version);control_json_string(mode,sizeof mode,m->boot_mode);
    r->status=200;snprintf(r->json,sizeof r->json,
      "{\"platform\":\"anyka\",\"firmware_version\":%s,\"boot_mode\":%s,\"video\":{\"width\":%u,\"height\":%u,\"fps\":%u,\"frames\":%u},\"settings\":{\"brightness\":%d,\"shutter_ms\":%u.%03u,\"mains_hz\":%u,\"microphone\":%s,\"mic_gain\":%u},\"stream\":{\"running\":%s,\"last_error\":%d,\"source_error\":%d},\"audio_blocks\":%u,\"capabilities\":{\"scalar_picture\":%s,\"rotation\":%s,\"timestamp\":%s,\"audio_controls\":%s,\"manual_exposure\":%s}}",
      version,mode,m->options.width,m->options.height,m->options.fps,atomic_load(&m->frames),m->desired.effects[1]+100,(unsigned)m->desired.advanced.max_exposure_us/1000u,(unsigned)m->desired.advanced.max_exposure_us%1000u,m->desired.mains_hz,m->desired.microphone?"true":"false",m->desired.mic_gain,
      !source_error&&last&&now_seconds()-last<5?"true":"false",source_error,source_error,atomic_load(&m->audio_blocks),
      m->options.capabilities&1?"true":"false",m->options.capabilities&2?"true":"false",m->options.capabilities&4?"true":"false",m->options.capabilities&8?"true":"false",m->options.capabilities&16?"true":"false");
}
static int known_fields(const char *body,const char *path)
{
    const char *allowed=!strcmp(path,"/api/picture")?"|action|hue|brightness_offset|saturation_offset|contrast_offset|sharpness_offset|wdr_offset|mains_hz|auto_exposure|max_exposure_us|min_exposure_us|exposure_us|gain_q8|min_gain_q8|max_gain_q8|auto_white_balance|wb_red_q8|wb_green_q8|wb_blue_q8|temporal_nr|gamma_lift|encoder_target_kbps|encoder_maximum_kbps|encoder_gop|encoder_qp|encoder_min_qp|encoder_max_qp|encoder_rate_control|":!strcmp(path,"/api/rotation")?"|rotate_180|":"|brightness|shutter_ms|mains_hz|microphone|mic_gain|";
    const char *at=body;
    while(*at){const char *end=strchr(at,'&');if(!end)end=at+strlen(at);const char *equal=memchr(at,'=',(size_t)(end-at));if(!equal||equal==at||equal-at>64)return -EINVAL;char key[68];key[0]='|';memcpy(key+1,at,(size_t)(equal-at));key[1+equal-at]='|';key[2+equal-at]=0;if(!strstr(allowed,key))return -ENOTSUP;at=*end?end+1:end;if(*end&&! *at)return -EINVAL;}
    return 0;
}
static int parse_settings(struct anyka_management *m,const char *path,const char *body,struct anyka_management_settings *s)
{
    int value,rc=known_fields(body,path);if(rc)return rc;unsigned old_audio=s->microphone,old_gain=s->mic_gain;
    if(!strcmp(path,"/api/picture")) {
        char action[32];rc=control_form(body,"action",action,sizeof action);if(rc<0)return -EINVAL;
        if(rc){if(strcmp(action,"defaults"))return -EINVAL;for(unsigned i=0;i<6;i++)s->effects[i]=0;s->mains_hz=50;if(m->options.capabilities&ANYKA_MANAGEMENT_ADVANCED){anyka_advanced_defaults(&s->advanced);s->advanced.configured=1;}}
        if(rc)anyka_advanced_clamp(&s->advanced,m->options.fps);
        if(rc&&(m->options.capabilities&ANYKA_MANAGEMENT_ENCODER)){if(!m->encoder_ready)return -ENOTSUP;s->encoder=(struct anyka_encoder_settings){512,1024,30,35,25,45,1};s->encoder_configured=1;}
        for(unsigned i=0;i<6;i++){rc=integer(body,keys[i],&value);if(rc<0)return -EINVAL;if(rc)s->effects[i]=value;}
        for(unsigned i=0;i<sizeof advanced_fields/sizeof advanced_fields[0];i++){rc=integer(body,advanced_fields[i].name,&value);if(rc<0)return -EINVAL;if(rc){if(!(m->options.capabilities&ANYKA_MANAGEMENT_ADVANCED))return -ENOTSUP;memcpy((unsigned char *)&s->advanced+advanced_fields[i].offset,&value,sizeof value);s->advanced.configured=1;}}
        for(unsigned i=0;i<sizeof encoder_fields/sizeof encoder_fields[0];i++){rc=integer(body,encoder_fields[i].name,&value);if(rc<0)return -EINVAL;if(rc){if(value<0)return -EINVAL;if(!(m->options.capabilities&ANYKA_MANAGEMENT_ENCODER)||!m->encoder_ready)return -ENOTSUP;unsigned positive=(unsigned)value;memcpy((unsigned char *)&s->encoder+encoder_fields[i].offset,&positive,4);s->encoder_configured=1;}}
    }else if(!strcmp(path,"/api/rotation")) {
        if(boolean(body,"rotate_180",&s->rotate_180)!=1)return -EINVAL;
    }else {
        rc=integer(body,"brightness",&value);if(rc<0)return -EINVAL;if(rc){if(value<50||value>150)return -EINVAL;s->effects[1]=value-100;}
        rc=integer(body,"shutter_ms",&value);if(rc<0)return -EINVAL;if(rc){if(!(m->options.capabilities&ANYKA_MANAGEMENT_ADVANCED)){if(value!=10)return -ENOTSUP;}else{if(value<10||value>60)return -EINVAL;s->advanced.max_exposure_us=value*1000;if(s->advanced.min_exposure_us>s->advanced.max_exposure_us)s->advanced.min_exposure_us=s->advanced.max_exposure_us;s->advanced.configured=1;}}
        rc=boolean(body,"microphone",&s->microphone);if(rc<0)return -EINVAL;
        rc=integer(body,"mic_gain",&value);if(rc<0)return -EINVAL;if(rc){if(value<0)return -EINVAL;s->mic_gain=(unsigned)value;}
    }
    rc=integer(body,"mains_hz",&value);if(rc<0)return -EINVAL;if(rc){if(value<0)return -EINVAL;s->mains_hz=(unsigned)value;}
    if(!(m->options.capabilities&ANYKA_MANAGEMENT_AUDIO)&&(s->microphone!=old_audio||s->mic_gain!=old_gain))return -ENOTSUP;
    if(!(m->options.capabilities&ANYKA_MANAGEMENT_SCALARS)&&(memcmp(s->effects,m->desired.effects,sizeof s->effects)||s->mains_hz!=m->desired.mains_hz))return -ENOTSUP;
    if(!(m->options.capabilities&ANYKA_MANAGEMENT_ROTATION)&&s->rotate_180!=m->desired.rotate_180)return -ENOTSUP;
    unsigned maximum=anyka_advanced_max_us(m->options.fps);
    if((unsigned)s->advanced.max_exposure_us>maximum||(unsigned)s->advanced.exposure_us>maximum)return -EINVAL;
    return valid(s)?0:-EINVAL;
}
void anyka_management_http_backend(void *opaque,const struct control_request *request,struct control_reply *reply)
{
    struct anyka_management *m=opaque;if(!m||!request||!request->path||!request->method){error_reply(reply,500,"Management unavailable");return;}
    int get=!strcmp(request->method,"GET"),post=!strcmp(request->method,"POST");
    if(!get&&!post){error_reply(reply,405,"Method not supported");return;}
    const char *path=request->path;
    int settings_path=!strcmp(path,"/api/settings")||!strcmp(path,"/api/picture")||!strcmp(path,"/api/rotation");
    if(!strcmp(path,"/api/picture/keyframe")){if(!post){error_reply(reply,405,"POST required");return;}pthread_mutex_lock(&m->lock);int ready=m->encoder_ready;pthread_mutex_unlock(&m->lock);if(!ready||!camera_request_keyframe){error_reply(reply,503,"Encoder control unavailable");return;}camera_request_keyframe();reply->status=202;strcpy(reply->json,"{\"queued\":true}");return;}
    int own=settings_path||!strcmp(path,"/api/status")||!strcmp(path,"/api/display")||!strcmp(path,"/api/credentials")||(!strcmp(path,"/api/preview/status")&&m->preview);
    if(!own){if(m->options.delegate)m->options.delegate(m->options.delegate_opaque,request,reply);else error_reply(reply,404,"Control unavailable on this platform");return;}
    if(!strcmp(path,"/api/credentials")) {
        if(get){char web[64],stream_user[64],a[400],b[400];if(camera_credentials_user(0,web,sizeof web)||camera_credentials_user(1,stream_user,sizeof stream_user)){error_reply(reply,503,"Credentials unavailable");return;}control_json_string(a,sizeof a,web);control_json_string(b,sizeof b,stream_user);reply->status=200;snprintf(reply->json,sizeof reply->json,"{\"web_username\":%s,\"stream_username\":%s}",a,b);return;}
        char body[1024],role[16],user[64],password[128],confirm[128];
        if(control_body_text(&request->body,body,sizeof body)||control_form(body,"service",role,sizeof role)!=1||control_form(body,"username",user,sizeof user)!=1||control_form(body,"password",password,sizeof password)!=1||control_form(body,"confirm",confirm,sizeof confirm)!=1||strcmp(password,confirm)||(strcmp(role,"web")&&strcmp(role,"stream"))){error_reply(reply,400,"Invalid credential request");return;}
        int rc=camera_credentials_change(!strcmp(role,"stream"),user,password);memset(password,0,sizeof password);memset(confirm,0,sizeof confirm);memset(body,0,sizeof body);
        if(rc){error_reply(reply,rc==-EINVAL?400:500,"Could not save credentials");return;}reply->status=200;strcpy(reply->json,"{\"message\":\"Credentials saved\"}");return;
    }
    pthread_mutex_lock(&m->lock);
    if(get){
        if(!strcmp(path,"/api/status")||!strcmp(path,"/api/settings"))status_reply(m,reply);
        else if(!strcmp(path,"/api/picture"))picture_reply(m,reply);
        else if(!strcmp(path,"/api/rotation"))rotation_reply(m,reply);
        else if(!strcmp(path,"/api/display"))display_reply(m,reply);
        else {char mime[160];control_json_string(mime,sizeof mime,preview_mime(m->preview));reply->status=200;snprintf(reply->json,sizeof reply->json,"{\"available\":true,\"mime\":%s}",mime);}
        pthread_mutex_unlock(&m->lock);return;
    }
    if(!settings_path&&strcmp(path,"/api/display")){pthread_mutex_unlock(&m->lock);error_reply(reply,405,"Read-only API");return;}
    if(pending(m)){pthread_mutex_unlock(&m->lock);error_reply(reply,409,"A settings change is being applied; retry shortly");return;}
    char body[4096];if(control_body_text(&request->body,body,sizeof body)){pthread_mutex_unlock(&m->lock);error_reply(reply,400,"Invalid form body");return;}
    if(!strcmp(path,"/api/display")) {
        struct camera_display_settings next=m->display;char zone[40],name[64];int rc;
        rc=control_form(body,"camera_name",name,sizeof name);
        if(rc==1)memcpy(next.name,name,strlen(name)+1);
        if(rc<0||boolean(body,"timestamp_enabled",&next.timestamp_enabled)<0){pthread_mutex_unlock(&m->lock);error_reply(reply,400,"Invalid display values");return;}
        rc=control_form(body,"timezone",zone,sizeof zone);
        if(rc<0||(rc&&strcmp(zone,"UTC")&&strcmp(zone,"Europe/Amsterdam"))){pthread_mutex_unlock(&m->lock);error_reply(reply,400,"Unsupported timezone");return;}
        if(rc)next.timezone=!strcmp(zone,"UTC");
        if(!camera_display_valid(&next)){pthread_mutex_unlock(&m->lock);error_reply(reply,400,"Invalid camera name");return;}
        if(next.timestamp_enabled&&!(m->options.capabilities&ANYKA_MANAGEMENT_TIMESTAMP)){pthread_mutex_unlock(&m->lock);error_reply(reply,501,"Video timestamp is not supported yet");return;}
        if(camera_display_save(m->directory,&next)){camera_display_load(m->directory,&m->display);pthread_mutex_unlock(&m->lock);error_reply(reply,500,"Could not durably save display settings");return;}
        m->display=next;queue(m);display_reply(m,reply);
    }else {
        struct anyka_management_settings next=m->desired;int rc=parse_settings(m,path,body,&next);
        if(rc){pthread_mutex_unlock(&m->lock);error_reply(reply,rc==-ENOTSUP?501:400,rc==-ENOTSUP?"This control is not implemented for this hardware":"Invalid picture settings");return;}
        rc=save_settings(m->directory,&next);
        if(rc){load_settings(m->directory,&m->desired);pthread_mutex_unlock(&m->lock);error_reply(reply,500,"Could not durably save picture settings");return;}
        m->desired=next;queue(m);
        if(!strcmp(path,"/api/picture"))picture_reply(m,reply);else if(!strcmp(path,"/api/rotation"))rotation_reply(m,reply);else status_reply(m,reply);
    }
    pthread_mutex_unlock(&m->lock);
}
int anyka_management_open(struct anyka_management **out,int dir,const struct anyka_management_options *options)
{
    if(!out)return -EINVAL;
    *out=NULL;
    struct stat st;if(!options||!options->version||!options->boot_mode||!options->initial_credentials||dir<0||fstat(dir,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||strlen(options->version)>=64||strlen(options->boot_mode)>=64)return -EINVAL;
    struct anyka_management *m=calloc(1,sizeof *m);if(!m)return -ENOMEM;
    m->directory=fcntl(dir,F_DUPFD_CLOEXEC,3);if(m->directory<0){free(m);return -errno;}
    struct stat temporary;
    if(!fstatat(dir,"anyka-picture.tmp",&temporary,AT_SYMLINK_NOFOLLOW)){
        if(!S_ISREG(temporary.st_mode)||temporary.st_uid!=geteuid()||(temporary.st_mode&0777)!=0600||temporary.st_nlink!=1||temporary.st_size>136||unlinkat(dir,"anyka-picture.tmp",0)){close(m->directory);free(m);return -EPROTO;}
    }else if(errno!=ENOENT){close(m->directory);free(m);return -errno;}
    int rc=load_settings(dir,&m->desired);if(!rc&&camera_display_load(dir,&m->display))rc=-EPROTO;
    if(!rc)rc=camera_credentials_open(dir,options->initial_credentials);
    if(rc){close(m->directory);free(m);return rc;}
    anyka_advanced_clamp(&m->desired.advanced,options->fps);
    m->options=*options;strcpy(m->version,options->version);strcpy(m->boot_mode,options->boot_mode);
    /* Credentials are now owned by the shared credential store; do not retain caller strings. */
    m->options.initial_credentials=NULL;m->options.version=m->version;m->options.boot_mode=m->boot_mode;
    rc=pthread_mutex_init(&m->lock,NULL);if(rc){close(m->directory);free(m);return -rc;}
    if(options->media.sps&&options->media.pps){struct preview_config pc={options->width,options->height,options->media.sps,options->media.pps,options->media.sps_size,options->media.pps_size};m->preview=preview_create(&pc);if(!m->preview){pthread_mutex_destroy(&m->lock);close(m->directory);free(m);return -EINVAL;}}
    atomic_init(&m->frames,0);atomic_init(&m->audio_blocks,0);atomic_init(&m->last_frame_second,0);atomic_init(&m->preview_drops,0);atomic_init(&m->source_error,0);
    m->revision=1;*out=m;return 0;
}
int anyka_management_take(struct anyka_management *m,struct anyka_management_command *command)
{
    if(!m||!command||pthread_mutex_trylock(&m->lock))return 0;
    int ready=pending(m)&&!m->inflight;
    if(ready){command->revision=m->revision;command->settings=m->desired;command->display=m->display;m->inflight=m->revision;}
    pthread_mutex_unlock(&m->lock);return ready;
}
void anyka_management_complete(struct anyka_management *m,unsigned revision,int error)
{
    if(!m)return;
    pthread_mutex_lock(&m->lock);
    if(revision&&revision==m->inflight&&revision==m->revision){m->completed=revision;m->inflight=0;m->last_error=error;}
    pthread_mutex_unlock(&m->lock);
}
int anyka_management_settings(struct anyka_management *m,struct anyka_management_settings *settings)
{if(!m||!settings)return -EINVAL;pthread_mutex_lock(&m->lock);*settings=m->desired;pthread_mutex_unlock(&m->lock);return 0;}
int anyka_management_preview(void *opaque,int fd)
{struct anyka_management *m=opaque;return m&&m->preview?preview_stream(m->preview,fd):-ENOTSUP;}
int anyka_management_video(struct anyka_management *m,const struct camera_media_frame *frame)
{if(!m||!frame||!frame->data||!frame->size)return 0;atomic_fetch_add(&m->frames,1);atomic_store(&m->last_frame_second,now_seconds());if(!m->preview)return 0;int rc=preview_publish(m->preview,frame->data,frame->size,frame->pts_us,frame->keyframe);if(rc)atomic_fetch_add(&m->preview_drops,1);return rc;}
void anyka_management_audio(struct anyka_management *m,const struct camera_media_frame *frame)
{if(m&&frame&&frame->data&&frame->size)atomic_fetch_add(&m->audio_blocks,1);}
void anyka_management_source_error(struct anyka_management *m,int error)
{if(m&&error<0){int expected=0;atomic_compare_exchange_strong(&m->source_error,&expected,error);}}

void anyka_management_encoder_state(struct anyka_management *m,const struct anyka_encoder_snapshot *state,int ready)
{
    if(!m||!state)return;
    pthread_mutex_lock(&m->lock);m->encoder_state=*state;m->encoder_ready=ready&&(m->options.capabilities&ANYKA_MANAGEMENT_ENCODER);
    if(m->encoder_ready&&!m->desired.encoder_configured)m->desired.encoder=state->settings;
    pthread_mutex_unlock(&m->lock);
}
