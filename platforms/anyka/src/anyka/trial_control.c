/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "trial_control.h"
#include "../native/control/http.h"
#include "../preview/preview.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct anyka_trial_control {
    struct preview *preview;
    char bind[16], credentials[701], name[128], version[64];
    unsigned width,height,fps,audio_rate;
    enum camera_audio_codec audio_codec;
    atomic_uint video_frames,audio_blocks,preview_drops,last_video_second;
    atomic_int source_error;
    unsigned started;
};
static unsigned seconds_now(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned)t.tv_sec;}
static void backend(void *opaque,const struct control_request *request,struct control_reply *reply)
{
    struct anyka_trial_control *c=opaque;
    if(strcmp(request->method,"GET")) {
        reply->status=405;snprintf(reply->json,sizeof reply->json,"{\"error\":\"Experimental trial is read-only; settings and firmware updates are unavailable\"}");return;
    }
    char name[768],version[400],mime[160];
    if(control_json_string(name,sizeof name,c->name)<0 ||
       control_json_string(version,sizeof version,c->version)<0 ||
       control_json_string(mime,sizeof mime,preview_mime(c->preview))<0) {
        reply->status=500;strcpy(reply->json,"{\"error\":\"Status serialization failed\"}");return;
    }
    if(!strcmp(request->path,"/api/preview/status")) {
        reply->status=200;snprintf(reply->json,sizeof reply->json,
            "{\"available\":true,\"mime\":%s,\"audio\":false,\"max_clients\":2}",mime);return;
    }
    if(strcmp(request->path,"/api/status")) {
        reply->status=404;strcpy(reply->json,"{\"error\":\"Unsupported trial API\"}");return;
    }
    unsigned now=seconds_now(),last=atomic_load(&c->last_video_second);
    int error=atomic_load(&c->source_error);
    reply->status=200;
    snprintf(reply->json,sizeof reply->json,
        "{\"camera_name\":%s,\"firmware_version\":%s,\"boot_mode\":\"sd_ram_sdk_trial\","
        "\"uptime_seconds\":%u,\"video\":{\"width\":%u,\"height\":%u,\"configured_fps\":%u,\"frames\":%u,\"age_seconds\":%u},"
        "\"audio\":{\"codec\":\"%s\",\"rate\":%u,\"channels\":1,\"blocks\":%u},"
        "\"stream\":{\"running\":%s,\"last_error\":%d,\"port\":8554,\"path\":\"/native\"},"
        "\"preview_drops\":%u,\"capabilities\":{\"preview\":true,\"settings\":false,\"ptz\":false,\"wifi\":false,\"updates\":false}}",
        name,version,now-c->started,c->width,c->height,c->fps,atomic_load(&c->video_frames),last?now-last:now-c->started,
        c->audio_codec==CAMERA_AUDIO_L16?"L16":c->audio_codec==CAMERA_AUDIO_PCMA?"PCMA":"none",c->audio_rate,
        atomic_load(&c->audio_blocks),!error&&last&&now-last<5?"true":"false",error,atomic_load(&c->preview_drops));
}
static int stream(void *opaque,int fd)
{return preview_stream(((struct anyka_trial_control *)opaque)->preview,fd);}
static int copy_string(char *out,size_t capacity,const char *input)
{if(!input||strlen(input)>=capacity)return -EINVAL;memcpy(out,input,strlen(input)+1);return 0;}
int anyka_trial_control_start(struct anyka_trial_control **out,const struct anyka_trial_control_config *config)
{
    if(!out)return -EINVAL;
    *out=NULL;
    if(!config||!config->width||!config->height||!config->fps||!config->port)return -EINVAL;
    struct anyka_trial_control *c=calloc(1,sizeof *c);if(!c)return -ENOMEM;
    if(copy_string(c->bind,sizeof c->bind,config->bind_address)||
       copy_string(c->credentials,sizeof c->credentials,config->credentials)||
       copy_string(c->name,sizeof c->name,config->camera_name)||
       copy_string(c->version,sizeof c->version,config->version)) {free(c);return -EINVAL;}
    c->width=config->width;c->height=config->height;c->fps=config->fps;
    c->audio_codec=config->media.audio_codec;c->audio_rate=config->media.audio_rate;c->started=seconds_now();
    atomic_init(&c->video_frames,0);atomic_init(&c->audio_blocks,0);atomic_init(&c->preview_drops,0);
    atomic_init(&c->last_video_second,0);atomic_init(&c->source_error,0);
    struct preview_config pc={c->width,c->height,config->media.sps,config->media.pps,config->media.sps_size,config->media.pps_size};
    c->preview=preview_create(&pc);if(!c->preview){free(c);return -EINVAL;}
    struct control_http_config hc={c->bind,c->credentials,config->port,c,backend,stream};
    int rc=control_http_start(&hc);
    if(rc){preview_destroy(c->preview);free(c);return rc;}
    *out=c;return 0;
}
int anyka_trial_control_video(struct anyka_trial_control *c,const struct camera_media_frame *frame)
{
    if(!c)return 0;
    if(!frame||!frame->data||!frame->size)return -EINVAL;
    atomic_fetch_add(&c->video_frames,1);atomic_store(&c->last_video_second,seconds_now());
    int rc=preview_publish(c->preview,frame->data,frame->size,frame->pts_us,frame->keyframe);
    if(rc)atomic_fetch_add(&c->preview_drops,1);
    return rc;
}
void anyka_trial_control_audio(struct anyka_trial_control *c,const struct camera_media_frame *frame)
{if(c&&frame&&frame->data&&frame->size)atomic_fetch_add(&c->audio_blocks,1);}
void anyka_trial_control_error(struct anyka_trial_control *c,int error)
{if(c&&error<0){int expected=0;atomic_compare_exchange_strong(&c->source_error,&expected,error);}}
