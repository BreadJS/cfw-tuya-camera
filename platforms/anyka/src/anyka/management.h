/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_MANAGEMENT_H
#define CAMERA_ANYKA_MANAGEMENT_H
#include "../native/control/http.h"
#include "../native/rtsp/camera_rtsp.h"
#include "../display/settings.h"
#include "advanced_picture.h"
#include "encoder_control.h"
#define ANYKA_MANAGEMENT_SCALARS 1u
#define ANYKA_MANAGEMENT_ROTATION 2u
#define ANYKA_MANAGEMENT_TIMESTAMP 4u
#define ANYKA_MANAGEMENT_AUDIO 8u
#define ANYKA_MANAGEMENT_ADVANCED 16u
#define ANYKA_MANAGEMENT_ENCODER 32u
struct anyka_management;
struct anyka_management_settings {
    int effects[6]; /* hue, AE brightness offset, saturation, contrast, sharp, WDR; -50..50 */
    unsigned mains_hz,rotate_180,microphone,mic_gain;
    struct anyka_advanced_settings advanced;
    unsigned encoder_configured;
    struct anyka_encoder_settings encoder;
};
struct anyka_management_command {
    unsigned revision;
    struct anyka_management_settings settings;
    struct camera_display_settings display;
};
struct anyka_management_options {
    const char *version,*boot_mode,*initial_credentials;
    unsigned capabilities,width,height,fps;
    struct camera_media_description media;
    void *delegate_opaque;
    /* Network/OTA/board routes not handled here. Must set status or return404. */
    control_backend delegate;
};
/* Caller supplies private0700 owned config directory. Copies options/strings,
 * opens existing credential store, creates preview hub (no listener/hardware).
 * One startup snapshot is queued. Context outlives HTTP/media/executor threads.
 * Failure leaves *out NULL, but credentials store may have been opened. */
int anyka_management_open(struct anyka_management **out,int config_directory,
                           const struct anyka_management_options *);
void anyka_management_http_backend(void *,const struct control_request *,struct control_reply *);
int anyka_management_preview(void *,int accepted_fd);
int anyka_management_video(struct anyka_management *,const struct camera_media_frame *);
void anyka_management_audio(struct anyka_management *,const struct camera_media_frame *);
void anyka_management_source_error(struct anyka_management *,int error);
/* Dedicated owner/executor thread only. take uses trylock, never waits for an
 * HTTP/disk operation; 1=command copied+marked inflight,0=no work/lockbusy.
 * Apply SDK/board commands outside management locks. complete ignores stale
 * revisions. No automatic retries after failure. New POST can queue retry.
 * Caller decides individual capability implementation, preserving baseline
 * orientation and PTZ mapping. No SDK calls occur anywhere in this module. */
int anyka_management_take(struct anyka_management *,struct anyka_management_command *);
void anyka_management_complete(struct anyka_management *,unsigned revision,int error);
/* Executor publishes copied readback; no transport thread reads SDK state. */
void anyka_management_encoder_state(struct anyka_management *,const struct anyka_encoder_snapshot *,int ready);
int anyka_management_settings(struct anyka_management *,struct anyka_management_settings *);
#endif
