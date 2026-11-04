/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "picture_executor.h"
#include "management.h"
#include "audio_backend.h"
#include "picture_abi.h"
#include "advanced_picture.h"
#include <stdatomic.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct anyka_executor {
    struct anyka_management *management;
    struct anyka_board *board;
    struct anyka_audio *audio;
    struct anyka_advanced *advanced;
    struct anyka_encoder_control *encoder;
    unsigned encoder_handle,video_fps;
    int frame_limit_applied;
    int encoder_registered, encoder_attempted;
    uint64_t encoder_poll, encoder_next_idr;
    pthread_mutex_t mutex;
    pthread_t thread;
    struct anyka_board_status status;
    int status_error, paused, pause_requested, activated;
    unsigned serial, done, action;
    int value, result, pending;
    unsigned baseline_orientation, rotated;
    anyka_picture_effect_set_fn effect_set;
    anyka_picture_effect_get_fn effect_get;
    anyka_picture_switch_mode_fn switch_mode;
    anyka_picture_osd_draw_fn draw;
    anyka_picture_osd_clean_fn clean;
    struct camera_display_settings display;
    struct anyka_management_settings applied;
    int have_applied, osd_ready, osd_visible;
    time_t last_second;
    char font[256];
};
/* Every network callback only queues; the SDK stays on this worker. */
static atomic_uint keyframe_pending=ATOMIC_VAR_INIT(0);
void camera_request_keyframe(void){atomic_store(&keyframe_pending,1);}
static uint64_t milliseconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (unsigned)now.tv_nsec / 1000000u;
}
static int bind_symbol(void *out, size_t size, const char *name)
{
    void *address = dlsym(RTLD_DEFAULT, name);
    if (!address || size != sizeof address) return -ENOSYS;
    memcpy(out, &address, size);
    return 0;
}
#define BIND(member,name) do { int bind_rc=bind_symbol(&e->member,sizeof e->member,name); if(bind_rc)return bind_rc; } while(0)
static int sdk_result(int rc)
{
    if (rc) fprintf(stderr, "ANYKA_CONTROL_SDK error=0x%x\n", (unsigned)rc);
    return rc ? -EIO : 0;
}
static void snapshot(struct anyka_executor *e)
{
    struct anyka_board_status status;
    int rc = anyka_board_status(e->board, &status);
    pthread_mutex_lock(&e->mutex);
    if (!rc) e->status = status;
    e->status_error = rc;
    pthread_mutex_unlock(&e->mutex);
}
static int apply(struct anyka_executor *e, const struct anyka_management_command *command)
{
    const struct anyka_management_settings *s = &command->settings;
    if(e->video_fps&&!e->frame_limit_applied){
        int rc=0;if(!e->advanced)rc=anyka_advanced_open(&e->advanced);
        if(!rc)rc=anyka_advanced_limit_fps(e->advanced,e->video_fps);
        if(rc)return rc;
        e->frame_limit_applied=1;
    }
    for (unsigned i=0; i<6; i++) {
        if (!e->have_applied || s->effects[i] != e->applied.effects[i]) {
            int rc = sdk_result(e->effect_set(0, i, s->effects[i]));
            if (rc) return rc;
            int value = 999;
            rc = sdk_result(e->effect_get(0, i, &value));
            if (rc || value != s->effects[i]) return rc ? rc : -EIO;
        }
    }
    if (!e->have_applied || s->mains_hz != e->applied.mains_hz) {
        if (s->mains_hz != 50 && s->mains_hz != 60) return -EINVAL;
        int rc = sdk_result(e->effect_set(0, 7, (int)s->mains_hz));
        if (rc) return rc;
    }
    if (!e->have_applied || s->rotate_180 != e->applied.rotate_180) {
        unsigned orientation = e->baseline_orientation ^ (s->rotate_180 ? 3u : 0u);
        int rc = anyka_board_stop(e->board);
        if (rc) return rc;
        rc = sdk_result(e->effect_set(0, 8, (int)orientation));
        if (rc) return rc;
        int observed = -1;
        rc = sdk_result(e->effect_get(0, 8, &observed));
        if (rc || observed != (int)orientation) return rc ? rc : -EIO;
        e->rotated = s->rotate_180;
    }
    if(s->advanced.configured&&(!e->have_applied||memcmp(&s->advanced,&e->applied.advanced,sizeof s->advanced))){
        int rc=0;
        if(!e->advanced)rc=anyka_advanced_open(&e->advanced);
        if(!rc&&e->video_fps)rc=anyka_advanced_limit_fps(e->advanced,e->video_fps);
        if(!rc)rc=anyka_advanced_apply(e->advanced,&s->advanced);
        if(rc)return rc;
    }
    if(s->encoder_configured&&(!e->have_applied||!e->applied.encoder_configured||memcmp(&s->encoder,&e->applied.encoder,sizeof s->encoder))){
        if(!e->encoder)return -ENODEV;
        struct anyka_encoder_snapshot state={0};
        int rc=anyka_encoder_control_apply(e->encoder,ANYKA_ENCODER_ALL,&s->encoder,&state);
        anyka_management_encoder_state(e->management,&state,1);
        if(rc)return rc;
        atomic_store(&keyframe_pending,0);e->encoder_next_idr=milliseconds()+1000;
    }
    e->display = command->display;
    e->applied = *s;
    e->have_applied = 1;
    return 0;
}
static int board_command(struct anyka_executor *e, unsigned action, int value)
{
    switch (action) {
    case ANYKA_BOARD_DAY: {
        if (value != 0 && value != 1) return -EINVAL;
        /* Day moves the physical filter first; night changes the ISP first,
         * matching the original application's ordering. */
        int rc = 0;
        if (value) rc = anyka_board_day(e->board, 1);
        if (!rc) rc = sdk_result(e->switch_mode(0, value ? 0u : 1u));
        if (!rc && !value) rc = anyka_board_day(e->board, 0);
        if(!rc&&e->video_fps&&!e->advanced)rc=anyka_advanced_open(&e->advanced);
        if(!rc&&e->advanced){rc=anyka_advanced_rebaseline(e->advanced);if(!rc&&e->video_fps)rc=anyka_advanced_limit_fps(e->advanced,e->video_fps);if(!rc&&e->have_applied)rc=anyka_advanced_apply(e->advanced,&e->applied.advanced);}
        return rc;
    }
    case ANYKA_BOARD_IR: return anyka_board_ir(e->board, value);
    case ANYKA_BOARD_LED: return anyka_board_led(e->board, value);
    case ANYKA_BOARD_MOVE:
        if (value < 0 || value > 4) return -EINVAL;
        return anyka_board_move(e->board, (unsigned)value, 64, (int)e->rotated);
    case ANYKA_BOARD_HOLD: return anyka_board_hold(e->board,(unsigned)value,(int)e->rotated);
    case ANYKA_BOARD_STOP: return anyka_board_stop(e->board);
    default: return -EINVAL;
    }
}
static int osd_open(struct anyka_executor *e)
{
    anyka_picture_osd_init_fn init;
    anyka_picture_osd_font_file_fn font;
    anyka_picture_osd_rect_fn rect;
    anyka_picture_osd_font_size_fn size;
    anyka_picture_osd_color_fn color;
    const char *(*version)(void);
#define LOCAL(name,symbol) do{int rc=bind_symbol(&name,sizeof name,symbol);if(rc)return rc;}while(0)
    LOCAL(version,"ak_osd_get_version");
    if (strcmp(version(),ANYKA_PICTURE_OSD_VERSION)) return -EPROTO;
    LOCAL(version,"ak_osd_ex_get_version");
    if (strcmp(version(),ANYKA_PICTURE_OSD_EX_VERSION)) return -EPROTO;
    LOCAL(init,"ak_osd_ex_init"); LOCAL(font,"ak_osd_ex_set_font_file");
    LOCAL(rect,"ak_osd_ex_set_rect"); LOCAL(size,"ak_osd_ex_set_font_size");
    LOCAL(color,"ak_osd_ex_set_color");
    BIND(draw,"ak_osd_ex_draw_str"); BIND(clean,"ak_osd_ex_clean_str");
#undef LOCAL
    if (access(e->font,R_OK)) return -errno;
    int rc = sdk_result(font(16,e->font));
    if (!rc) rc = sdk_result(init());
    if (!rc) rc = sdk_result(color(1,0));
    if (!rc) rc = sdk_result(rect(0,0,8,8,768,64));
    if (!rc) rc = sdk_result(size(0,32));
    if (!rc) e->osd_ready=1;
    return rc;
}
static void timestamp(struct anyka_executor *e)
{
    if (!e->osd_ready) return;
    if (!e->display.timestamp_enabled) {
        if (e->osd_visible && !sdk_result(e->clean(0,0,0,0,768,64))) e->osd_visible=0;
        return;
    }
    time_t now=time(NULL);
    if (now==e->last_second) return;
    char text[24];
    if (camera_display_format_clock(now,e->display.timezone,text)) return;
    uint16_t wide[24];
    for(unsigned i=0;i<23;i++) wide[i]=(unsigned char)text[i];
    wide[23]=0;
    int rc=sdk_result(e->draw(0,0,0,0,wide,23));
    if(!rc){e->last_second=now;e->osd_visible=1;}
}
static void encoder_tick(struct anyka_executor *e,uint64_t now,int registered,unsigned handle)
{
    struct anyka_encoder_snapshot state={0};
    if(registered&&!e->encoder_attempted){
        e->encoder_attempted=1;
        int rc=anyka_encoder_control_open(&e->encoder,handle);
        if(!rc)rc=anyka_encoder_control_read(e->encoder,&state);
        if(rc)state.error=rc;
        anyka_management_encoder_state(e->management,&state,!rc);
        e->encoder_poll=now+1000;
    }
    if(!e->encoder)return;
    if(now>=e->encoder_next_idr&&atomic_exchange(&keyframe_pending,0)){
        int rc=anyka_encoder_request_idr(e->encoder);
        e->encoder_next_idr=now+1000;
        if(rc)fprintf(stderr,"ANYKA_ENCODER_IDR rc=%d\n",rc);
    }
    if(now>=e->encoder_poll){
        int rc=anyka_encoder_control_read(e->encoder,&state);
        if(rc)state.error=rc;
        anyka_management_encoder_state(e->management,&state,!rc);
        e->encoder_poll=now+1000;
    }
}
static void *worker(void *opaque)
{
    struct anyka_executor *e=opaque;
    for (;;) {
        pthread_mutex_lock(&e->mutex);
        int should_pause=e->pause_requested;
        unsigned action=e->action,serial=e->serial;
        int value=e->value,pending=e->pending,activated=e->activated;
        struct anyka_audio *audio=e->audio;
        int encoder_registered=e->encoder_registered;unsigned encoder_handle=e->encoder_handle;
        pthread_mutex_unlock(&e->mutex);
        if (should_pause) {
            (void)anyka_board_stop(e->board);
            snapshot(e);
            pthread_mutex_lock(&e->mutex);
            e->paused=1;
            pthread_mutex_unlock(&e->mutex);
            for (;;) pause();
        }
        if (pending) {
            int rc=board_command(e,action,value);
            snapshot(e);
            pthread_mutex_lock(&e->mutex);
            e->result=rc;e->done=serial;e->pending=0;
            pthread_mutex_unlock(&e->mutex);
        }
        if(activated)encoder_tick(e,milliseconds(),encoder_registered,encoder_handle);
        struct anyka_management_command command;
        if(activated&&anyka_management_take(e->management,&command)==1) {
            int rc=apply(e,&command);
            anyka_management_complete(e->management,command.revision,rc);
            fprintf(stderr,"ANYKA_SETTINGS_APPLIED revision=%u rc=%d\n",command.revision,rc);
        }
        if(audio&&e->have_applied)(void)anyka_audio_control(audio,e->applied.microphone,e->applied.mic_gain);
        anyka_board_tick(e->board,milliseconds());
        snapshot(e);
        if(activated)timestamp(e);
        usleep(20000);
    }
    return NULL;
}
int anyka_executor_open(struct anyka_executor **out,struct anyka_management *management,
                       struct anyka_board *board,const char *font_path,int enable_osd)
{
    if(!out||*out||!management||!board)return -EINVAL;
    struct anyka_executor *e=calloc(1,sizeof *e);
    if(!e)return -ENOMEM;
    e->management=management;e->board=board;
    const char *(*version)(void);
    int rc=bind_symbol(&version,sizeof version,"ak_vpss_get_version");
    if(rc)return rc;
    if(strcmp(version(),ANYKA_PICTURE_VPSS_VERSION))return -EPROTO;
    BIND(effect_set,"ak_vpss_effect_set"); BIND(effect_get,"ak_vpss_effect_get");
    BIND(switch_mode,"ak_vi_switch_mode");
    int orientation=-1;
    rc=sdk_result(e->effect_get(0,8,&orientation));
    if(rc||orientation<0||orientation>3)return rc?rc:-EPROTO;
    e->baseline_orientation=(unsigned)orientation;
    camera_display_defaults(&e->display);
    if(pthread_mutex_init(&e->mutex,NULL))return -ENOMEM;
    snapshot(e);
    if(enable_osd){
        if(!font_path||strlen(font_path)>=sizeof e->font)return -EINVAL;
        strcpy(e->font,font_path);
        rc=osd_open(e);
        fprintf(stderr,"ANYKA_OSD_OPEN rc=%d\n",rc);
        if(rc)return rc;
    }
    rc=pthread_create(&e->thread,NULL,worker,e);
    if(rc)return -rc;
    *out=e;
    return 0;
}
int anyka_executor_status(struct anyka_executor *e,struct anyka_board_status *out)
{
    if(!e||!out)return -EINVAL;
    pthread_mutex_lock(&e->mutex);
    *out=e->status;int rc=e->status_error;
    pthread_mutex_unlock(&e->mutex);
    return rc;
}
int anyka_executor_board(struct anyka_executor *e,enum anyka_board_action action,int value)
{
    if(!e)return -EINVAL;
    pthread_mutex_lock(&e->mutex);
    if(e->pending||e->pause_requested){pthread_mutex_unlock(&e->mutex);return -EBUSY;}
    unsigned serial=++e->serial;e->action=action;e->value=value;e->pending=1;
    pthread_mutex_unlock(&e->mutex);
    uint64_t deadline=milliseconds()+2000;
    while(milliseconds()<deadline){
        pthread_mutex_lock(&e->mutex);
        int done=e->done==serial,rc=e->result;
        pthread_mutex_unlock(&e->mutex);
        if(done)return rc;
        usleep(10000);
    }
    return -ETIMEDOUT;
}
int anyka_executor_pause(struct anyka_executor *e)
{
    if(!e)return -EINVAL;
    pthread_mutex_lock(&e->mutex);e->pause_requested=1;pthread_mutex_unlock(&e->mutex);
    uint64_t deadline=milliseconds()+2000;
    while(milliseconds()<deadline){
        pthread_mutex_lock(&e->mutex);int paused=e->paused;pthread_mutex_unlock(&e->mutex);
        if(paused)return 0;
        usleep(10000);
    }
    return -ETIMEDOUT;
}

void anyka_executor_set_audio(struct anyka_executor *e,struct anyka_audio *audio)
{if(e){pthread_mutex_lock(&e->mutex);e->audio=audio;pthread_mutex_unlock(&e->mutex);}}

void anyka_executor_activate(struct anyka_executor *e)
{if(e){pthread_mutex_lock(&e->mutex);e->activated=1;pthread_mutex_unlock(&e->mutex);}}

int anyka_executor_set_encoder(struct anyka_executor *e,unsigned handle)
{
    if(!e||handle>7)return -EINVAL;
    pthread_mutex_lock(&e->mutex);
    if(e->activated||e->encoder_registered){pthread_mutex_unlock(&e->mutex);return -EBUSY;}
    e->encoder_handle=handle;e->encoder_registered=1;
    pthread_mutex_unlock(&e->mutex);return 0;
}

int anyka_executor_set_video_fps(struct anyka_executor *e,unsigned fps)
{
    if(!e||(fps!=15&&fps!=25))return -EINVAL;
    pthread_mutex_lock(&e->mutex);if(e->activated){pthread_mutex_unlock(&e->mutex);return -EBUSY;}
    e->video_fps=fps;pthread_mutex_unlock(&e->mutex);return 0;
}
