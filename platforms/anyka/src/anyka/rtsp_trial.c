/* SPDX-License-Identifier: MIT
 * Experimental network trial only; no firmware/application autostart changes.
 */
#define _GNU_SOURCE
#include "media_backend.h"
#include "audio_backend.h"
#include "trial_control.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void retain(struct anyka_media_backend *backend, const char *stage, int rc)
{
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    printf("ANYKA_RTSP_HOLD stage=%s rc=%d source_error=%d frames=%llu resources_retained=yes cold_reboot_required=yes\n",
           stage, rc, anyka_media_error(backend), (unsigned long long)anyka_media_delivered(backend));
    fflush(stdout);
    for (;;) pause();
}

struct trial_av {
    struct camera_media_provider video;
    struct anyka_audio *audio;
    struct anyka_trial_control *control;
    uint64_t audio_logged;
};
static int trial_video(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct trial_av *av=opaque;
    int rc=av->video.video(av->video.opaque,now,frame);
    if(rc==1)(void)anyka_trial_control_video(av->control,frame);
    else if(rc<0)anyka_trial_control_error(av->control,-EIO);
    return rc;
}
static int trial_audio(void *opaque,uint64_t now,struct camera_media_frame *frame)
{
    struct trial_av *av=opaque;
    int rc=anyka_audio_read(av->audio,now,frame);
    if(rc==1)anyka_trial_control_audio(av->control,frame);
    else if(rc<0)anyka_trial_control_error(av->control,-EIO);
    struct anyka_audio_status status;
    if(!anyka_audio_status(av->audio,&status)&&
       (rc<0||status.blocks>av->audio_logged+150u||(!av->audio_logged&&status.clock_valid))){
        printf("ANYKA_MEDIA_AUDIO blocks=%llu samples=%llu clock_valid=%u error=%d sdk_result=%d source_ms=%llu gaps=%llu last_gap_us=%llu\n",
            (unsigned long long)status.blocks,(unsigned long long)status.samples,status.clock_valid,status.error,status.sdk_result,
            (unsigned long long)status.last_source_ms,(unsigned long long)status.discontinuities,
            (unsigned long long)status.last_gap_us);
        fflush(stdout);av->audio_logged=status.blocks;
    }
    return rc;
}

int camera_anyka_rtsp_trial(void)
{
    struct anyka_media_backend *backend = NULL;
    struct camera_media_provider provider;
    char error[128] = {0};
    /* Startup must also survive terminal teardown while hardware is opening.
     * The transport replaces INT/TERM handlers while serving, then retain()
     * restores retention behavior rather than closing unreviewed SDK state. */
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    int rc = anyka_media_open(&backend, &provider, error, sizeof error);
    if (rc) {
        printf("ANYKA_RTSP_START_FAILED rc=%d detail=%s\n", rc, error); fflush(stdout);
        if (anyka_media_hardware_attempted(backend)) retain(backend, "media-open", rc);
        return rc;
    }
    struct trial_av av = {.video=provider};
    rc=anyka_audio_open(&av.audio);
    if(rc){
        struct anyka_audio_status status={0};anyka_audio_status(av.audio,&status);
        printf("ANYKA_RTSP_AUDIO_OPEN_FAILED rc=%d sdk_result=%d\n",rc,status.sdk_result);
        fflush(stdout);retain(backend,"audio-open",rc);
    }
    provider.opaque=&av;provider.video=trial_video;provider.audio=trial_audio;
    provider.description.audio_codec=CAMERA_AUDIO_L16;
    provider.description.audio_rate=ANYKA_AUDIO_RATE;
    struct anyka_trial_control_config web={"0.0.0.0","admin:camera123","PTZ camera",
        "experimental SDK trial",81,1920,1080,15,provider.description};
    int web_rc=anyka_trial_control_start(&av.control,&web);
    printf("ANYKA_HTTP_TRIAL port=81 rc=%d read_only=yes\n",web_rc);
    fflush(stdout);
    struct camera_rtsp_config config = {"0.0.0.0", 8554, "/native", "admin:camera123", 4, 5000};
    printf("ANYKA_RTSP_READY port=8554 path=/native video=1920x1080 audio=L16-16000-mono experimental=yes\n");
    fflush(stdout);
    rc = camera_rtsp_run(&config, &provider);
    retain(backend, "RTSP-returned", rc);
    return rc;
}

#ifdef CAMERA_ANYKA_RTSP_TRIAL_CONSTRUCTOR
__attribute__((constructor)) static void rtsp_constructor(void)
{
    int rc = camera_anyka_rtsp_trial();
    fflush(stdout); _exit(rc ? 125 : 0);
}
#endif
