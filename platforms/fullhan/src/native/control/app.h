#ifndef CAMERA_CONTROL_APP_H
#define CAMERA_CONTROL_APP_H
#include "http.h"
#include "../audio/source.h"
#include "../../display/settings.h"
#include "../../preview/preview.h"
#include "../../picture/settings.h"
#include <sys/types.h>
struct control_settings {
    unsigned brightness,shutter_ms,mains_hz,microphone,mic_gain;
};
struct control_app;
struct control_illumination_ops {
    void *opaque;
    int (*apply)(void *,unsigned night,unsigned ir_led);
    int (*status_json)(void *,char *,size_t);
};
struct control_indicator_ops {
    void *opaque;
    int (*apply)(void *,unsigned mode); /*0off,1red,2green,3both;verifiedboardmapping*/
    int (*status_json)(void *,char *,size_t);
};
struct control_display_ops {
    void *opaque;
    int (*apply)(void *,unsigned enabled,const char *clock_text);
};
struct control_app_options {
    const char *persistent_directory,*ram_directory,*public_key;
    const char *wifi_config,*wifi_socket;
    const char *credentials;
    pid_t dhcp_pid;
    unsigned width,height,fps;
};
/* Creates only own private storage. Production persistent parent must be this
 * camera's mounted JFFS2 config partition. Context lives until board reset. */
int control_app_open(struct control_app **,const struct control_app_options *,
                     const struct control_settings *defaults);
int control_app_settings(struct control_app *,struct control_settings *);
void control_app_color_telemetry(struct control_app *,uint64_t,unsigned,unsigned,unsigned,unsigned,int);
void control_app_telemetry(struct control_app *,unsigned brightness_q16,unsigned exposure_lines,unsigned gain_q6);
/* Actual capture callbacks only; never exposed as an HTTP health assertion. */
void control_app_media_progress(struct control_app *,unsigned video_frames,int audio_healthy);
void control_app_stream_state(struct control_app *,int running,int result,int source_error);
int control_app_rotation_requested(struct control_app *,unsigned *rotate_180);
void control_app_rotation_telemetry(struct control_app *,unsigned applied_180,int error);
int control_app_picture_settings(struct control_app *,struct camera_picture_settings *,uint64_t *generation);
void control_app_picture_telemetry(struct control_app *,uint64_t generation,int error);
void control_app_encoder_active(struct control_app *,const struct fh_encoder_settings *);
int control_app_audio(struct control_app *,struct fh_audio_source *);
int control_app_illumination(struct control_app *,const struct control_illumination_ops *);
int control_app_indicator(struct control_app *,const struct control_indicator_ops *);
int control_app_display(struct control_app *,const struct control_display_ops *);
void control_app_display_tick(struct control_app *);
void control_app_display_error(struct control_app *,int);
int control_app_display_settings(struct control_app *,struct camera_display_settings *);
int control_app_preview(struct control_app *,struct preview *);
int control_app_start(struct control_app *,const char *bind_address,unsigned port);
void control_app_backend(void *,const struct control_request *,struct control_reply *);
#endif
