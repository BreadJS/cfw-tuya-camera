/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_TRIAL_CONTROL_H
#define CAMERA_ANYKA_TRIAL_CONTROL_H
#include "../native/rtsp/camera_rtsp.h"
struct anyka_trial_control;
struct anyka_trial_control_config {
    const char *bind_address, *credentials, *camera_name, *version;
    unsigned port, width, height, fps;
    struct camera_media_description media;
};
/* Read-only experimental HTTP service. Caller already owns/initialized media.
 * Copies config strings and parameter sets. Start AFTER first owned IDR and
 * final audio description. Success retains context for process lifetime (HTTP
 * has no stop API). Failure opens no hardware and leaves *out NULL. */
int anyka_trial_control_start(struct anyka_trial_control **out,
                              const struct anyka_trial_control_config *);
/* Call on the sole media thread after rc==1, BEFORE next provider callback.
 * Copies video synchronously; never waits for a client/statistics mutex. Preview
 * errors never change RTSP ownership or source outcome. Audio only counts data;
 * browser preview is video-only. NULL context is allowed (HTTP failed to start). */
int anyka_trial_control_video(struct anyka_trial_control *,const struct camera_media_frame *);
void anyka_trial_control_audio(struct anyka_trial_control *,const struct camera_media_frame *);
/* Sticky negative source error for status, no hardware action/SDK cleanup. */
void anyka_trial_control_error(struct anyka_trial_control *,int error);
#endif
