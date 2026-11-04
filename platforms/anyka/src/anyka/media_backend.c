/* SPDX-License-Identifier: MIT
 * Own capture/provider logic using the exact SDK functions exposed by the
 * fail-closed owner-supplied host. No vendor implementation code is copied.
 */
#define _GNU_SOURCE
#include "media_backend.h"
#include "sensor_timing.h"
#include "sdk_api.h"
#include "trial_guard.h"
#include "../native/rtsp/h264_parameters.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VI_POOL (13u * 1024u * 1024u)
#define VENC_POOL (10u * 1024u * 1024u)
#ifdef CAMERA_ANYKA_ENABLE_OSD
#define OSD_POOL (512u * 1024u)
#else
#define OSD_POOL 0u
#endif
#define PROFILE "/etc/config/isp_mis2006_mipi_2lane_h3b.conf"

struct anyka_media_backend {
    struct anyka_sdk_api api;
    struct anyka_channel destination;
    uint8_t *owned;
    uint8_t sps[CAMERA_RTSP_MAX_PARAMETER_SET], pps[CAMERA_RTSP_MAX_PARAMETER_SET];
    uint8_t initial_sps[CAMERA_RTSP_MAX_PARAMETER_SET], initial_pps[CAMERA_RTSP_MAX_PARAMETER_SET];
    size_t sps_bytes, pps_bytes, initial_sps_bytes, initial_pps_bytes, bytes;
    unsigned parameter_changes,parameter_pending,au_parameters;
    uint64_t pts_us, last_source_us, delivered;
    unsigned hardware_attempted, first_pending, ready, keyframe;
    int fault, sdk_error;
};

static int failed(struct anyka_media_backend *b, const char *stage, int rc)
{
    b->fault = rc < 0 ? rc : -EIO; b->sdk_error = rc;
    printf("ANYKA_MEDIA_FAULT stage=%s rc=%d resources_retained=%u\n", stage, rc, b->hardware_attempted);
    fflush(stdout); return b->fault;
}
static int checked(struct anyka_media_backend *b, const char *stage, int rc)
{
    printf("ANYKA_MEDIA_STAGE name=%s rc=%d monotonic_us=%llu\n", stage, rc,
           (unsigned long long)camera_monotonic_us());
    fflush(stdout); return rc ? failed(b, stage, rc) : 0;
}

static int start_code(const uint8_t *data, size_t bytes, size_t at, size_t *prefix)
{
    if (at + 3 > bytes || data[at] || data[at + 1]) return 0;
    if (data[at + 2] == 1) { *prefix = 3; return 1; }
    if (at + 4 <= bytes && !data[at + 2] && data[at + 3] == 1) { *prefix = 4; return 1; }
    return 0;
}
extern void camera_request_keyframe(void) __attribute__((weak));
/* Read bounded Exp-Golomb IDs from EBSP. Full SPS validation and the decoder
 * identity guard are shared with transport and preview. */
struct parameter_bits {uint8_t bytes[CAMERA_RTSP_MAX_PARAMETER_SET];size_t count,bit;};
static int parameter_bits(struct parameter_bits *r,const uint8_t *data,size_t bytes,size_t offset)
{
    if(!data||offset>=bytes||bytes>sizeof r->bytes)return -EBADMSG;
    r->count=r->bit=0;unsigned zeros=0;
    for(size_t i=offset;i<bytes;i++){
        unsigned value=data[i];
        if(zeros>=2&&value==3){if(i+1>=bytes||data[i+1]>3)return -EBADMSG;zeros=0;continue;}
        r->bytes[r->count++]=(uint8_t)value;zeros=value==0?zeros+1:0;
    }
    return 0;
}
static int parameter_bit(struct parameter_bits *r,unsigned *value)
{if(r->bit>=r->count*8)return -EBADMSG;*value=(r->bytes[r->bit/8]>>(7-r->bit%8))&1;r->bit++;return 0;}
static int parameter_ue(struct parameter_bits *r,unsigned *value)
{
    unsigned zeros=0,bit=0;int rc;
    while(!(rc=parameter_bit(r,&bit))&&!bit){if(++zeros>30)return -EBADMSG;}
    if(rc)return rc;
    unsigned suffix=0;
    for(unsigned i=0;i<zeros;i++){if(parameter_bit(r,&bit))return -EBADMSG;suffix=suffix*2+bit;}
    *value=((1u<<zeros)-1u)+suffix;return 0;
}
static int sps_id(const uint8_t *data,size_t bytes,unsigned *id)
{
    struct parameter_bits r;int rc=parameter_bits(&r,data,bytes,4);
    if(!rc)rc=parameter_ue(&r,id);
    return rc?rc:*id<=31?0:-EBADMSG;
}
static int parameter(struct anyka_media_backend *b,unsigned kind,const uint8_t *data,size_t bytes)
{
    if(!bytes||bytes>CAMERA_RTSP_MAX_PARAMETER_SET)return -EMSGSIZE;
    unsigned active_sps=0;
    if(kind==7){
        int rc=sps_id(data,bytes,&active_sps);if(rc)return rc;
        rc=camera_h264_sps_compatible(b->sps_bytes?b->sps:data,b->sps_bytes?b->sps_bytes:bytes,data,bytes);
        if(rc)return rc;
        if(b->sps_bytes==bytes&&!memcmp(b->sps,data,bytes))return 0;
        int changed=b->sps_bytes!=0;
        memcpy(b->sps,data,bytes);b->sps_bytes=bytes;
        if(changed){b->parameter_changes++;if(camera_request_keyframe)camera_request_keyframe();printf("ANYKA_MEDIA_SPS_UPDATE generation=%u bytes=%zu IDR_queued=%s\n",b->parameter_changes,bytes,camera_request_keyframe?"yes":"unavailable");fflush(stdout);}
        return 0;
    }
    if(!b->sps_bytes)return -EPROTO;
    int rc=sps_id(b->sps,b->sps_bytes,&active_sps);if(rc)return rc;
    struct parameter_bits r;unsigned picture_id=0,reference=0;
    rc=parameter_bits(&r,data,bytes,1);if(!rc)rc=parameter_ue(&r,&picture_id);if(!rc)rc=parameter_ue(&r,&reference);
    if(rc||picture_id>255||reference>31)return -EBADMSG;
    if(reference!=active_sps)return -EPROTO;
    if(b->pps_bytes==bytes&&!memcmp(b->pps,data,bytes))return 0;
    int changed=b->pps_bytes!=0;
    memcpy(b->pps,data,bytes);b->pps_bytes=bytes;
    if(changed){b->parameter_changes++;if(camera_request_keyframe)camera_request_keyframe();printf("ANYKA_MEDIA_PPS_UPDATE generation=%u bytes=%zu IDR_queued=%s\n",b->parameter_changes,bytes,camera_request_keyframe?"yes":"unavailable");fflush(stdout);}
    return 0;
}
static uint32_t parameter_crc(const uint8_t *data,size_t bytes)
{
    uint32_t crc=UINT32_MAX;
    for(size_t i=0;i<bytes;i++){crc^=data[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(UINT32_C(0xedb88320)&(0u-(crc&1u)));}
    return ~crc;
}
static void parameter_reject(const struct anyka_media_backend *b,unsigned kind,const uint8_t *data,size_t bytes,int rc)
{
    const uint8_t *old=kind==7?b->sps:b->pps;size_t old_bytes=kind==7?b->sps_bytes:b->pps_bytes;
    printf("ANYKA_MEDIA_PARAMETER_REJECT kind=%u rc=%d old_bytes=%zu new_bytes=%zu old_crc32=%08x new_crc32=%08x\n",kind,rc,old_bytes,bytes,parameter_crc(old,old_bytes),parameter_crc(data,bytes));
    fflush(stdout);
}
/* Every SPS retains identical decoder syntax through frame cropping. Validated
 * VUI/HRD updates and PPS changes referencing the active SPS stay in-band. */
static int inspect_au(struct anyka_media_backend *b, const uint8_t *data, size_t bytes,
                       unsigned *keyframe, unsigned *vcl)
{
    size_t at = 0; *keyframe = *vcl = 0;b->au_parameters=0;
    if (!bytes || bytes > CAMERA_RTSP_MAX_AU) return -EMSGSIZE;
    while (at < bytes) {
        size_t prefix;
        if (!start_code(data, bytes, at, &prefix)) return -EBADMSG;
        size_t begin = at + prefix;
        if (begin == bytes || (data[begin] & 128)) return -EBADMSG;
        unsigned kind = data[begin] & 31;
        if (!kind || kind > 23) return -EBADMSG;
        size_t end = begin + 1, next_prefix;
        while (end < bytes && !start_code(data, bytes, end, &next_prefix)) end++;
        int rc = 0;
        if (kind == 7) rc = parameter(b,7,data + begin,end - begin);
        if (kind == 8) rc = parameter(b,8,data + begin,end - begin);
        if (rc){parameter_reject(b,kind,data+begin,end-begin,rc);return rc;}
        if(kind==7)b->au_parameters|=1u;
        if(kind==8)b->au_parameters|=2u;
        if (kind == 1 || kind == 5) *vcl = 1;
        if (kind == 5) *keyframe = 1;
        at = end;
    }
    return 0;
}

static int source_clock(uint64_t source_ms, uint64_t last_us, uint64_t now_us, uint64_t *pts_us)
{
    if (source_ms > UINT64_MAX / 1000u) return -ERANGE;
    uint64_t pts = source_ms * 1000u;
    if (!pts || pts > now_us || now_us - pts > UINT64_C(5000000) ||
        (last_us && pts <= last_us)) return -ERANGE;
    *pts_us = pts; return 0;
}

static int pull(struct anyka_media_backend *b)
{
    if (b->fault) return -1;
    struct anyka_video_stream stream = {0};
    int rc = b->api.video_get_stream(&b->destination, &stream);
    if ((uint32_t)rc == UINT32_C(0x1a00000a)) return 0; /* Target's empty-queue code. */
    if (rc) { failed(b, "get-stream", rc); return -1; }
    int copy_error = 0;
    if (stream.bytes > CAMERA_RTSP_MAX_AU) copy_error = -EMSGSIZE;
    else if (!stream.bytes || !anyka_trial_readable(stream.data_address, stream.bytes)) copy_error = -EFAULT;
    else memcpy(b->owned, (const void *)(uintptr_t)stream.data_address, stream.bytes);
    uint32_t bytes = stream.bytes;
    uint64_t source_ms = stream.source_timestamp;
    /* The SDK's original descriptor is unchanged. Every successful get has
     * exactly one release, even if our copy/bounds check rejects the result. */
    rc = b->api.venc_release_stream(b->destination.channel, &stream);
    if (rc) { failed(b, "release-stream-once", rc); return -1; }
    if (copy_error) { failed(b, "stream-bounds", copy_error); return -1; }
    unsigned keyframe, vcl;
    unsigned parameter_generation=b->parameter_changes;
    rc = inspect_au(b, b->owned, bytes, &keyframe, &vcl);
    if (rc) { failed(b, "H264-parameters", rc); return -1; }
    if(b->parameter_changes!=parameter_generation)b->parameter_pending=1;
    if (!vcl) return 0;
    /* The SDK may deliver a parameter-only buffer. Carry a pending change into
     * the next owned VCL AU so transport/preview see it even in that case. */
    if(b->parameter_pending&&(b->au_parameters&3u)!=3u){
        size_t headers=b->sps_bytes+b->pps_bytes+8u;
        if(headers>CAMERA_RTSP_MAX_AU-bytes){failed(b,"parameter-prefix-bounds",-EMSGSIZE);return -1;}
        memmove(b->owned+headers,b->owned,bytes);
        const uint8_t prefix[4]={0,0,0,1};
        memcpy(b->owned,prefix,4);memcpy(b->owned+4,b->sps,b->sps_bytes);
        memcpy(b->owned+4+b->sps_bytes,prefix,4);memcpy(b->owned+8+b->sps_bytes,b->pps,b->pps_bytes);
        bytes+=(uint32_t)headers;
    }
    b->parameter_pending=0;
    uint64_t pts = 0, now = camera_monotonic_us();
    /* The successful capture trial established boot-monotonic milliseconds.
     * Recheck the relationship instead of inventing a frame-counter clock. */
    if (source_clock(source_ms, b->last_source_us, now, &pts)) {
        failed(b, "source-clock", -ERANGE); return -1;
    }
    b->last_source_us = pts; b->pts_us = pts; b->bytes = bytes; b->keyframe = keyframe;
    return 1;
}

static int video(void *opaque, uint64_t now_us, struct camera_media_frame *out)
{
    (void)now_us;
    struct anyka_media_backend *b = opaque;
    if (!b || !out) return -1;
    memset(out, 0, sizeof *out);
    if (b->fault || !b->ready) return -1;
    int rc;
    if (b->first_pending) { b->first_pending = 0; rc = 1; }
    else rc = pull(b);
    if (rc != 1) return rc;
    *out = (struct camera_media_frame){.data=b->owned,.size=b->bytes,.pts_us=b->pts_us,.keyframe=b->keyframe!=0};
    b->delivered++;
    if (b->delivered == 1 || !(b->delivered % 150)) {
        printf("ANYKA_MEDIA_VIDEO frames=%llu bytes=%zu pts_us=%llu keyframe=%u\n",
               (unsigned long long)b->delivered, b->bytes, (unsigned long long)b->pts_us, b->keyframe);
        fflush(stdout);
    }
    return 1;
}

int anyka_media_open_mode(struct anyka_media_backend **out, struct camera_media_provider *provider,
                      const struct anyka_video_mode *mode,char *error, size_t capacity)
{
    if (out) *out = NULL;
    if (provider) memset(provider, 0, sizeof *provider);
    if (!out || !provider || anyka_video_mode_validate(mode)) return -EINVAL;
    const char *gate = getenv("CAMERA_ANYKA_RTSP_TRIAL");
    const char *application = getenv("CAMERA_APPLICATION");
    if (((!gate || strcmp(gate, "1")) && (!application || strcmp(application,"1"))) || geteuid() != 0) return -EPERM;
    int rc = anyka_trial_safe_sdk_host(); if (rc) return rc;
    rc = anyka_trial_exclusive_owner(); if (rc) return rc;
    rc = anyka_trial_dma_budget(VI_POOL + OSD_POOL, VENC_POOL); if (rc) return rc;
    const char *profile_path = getenv("CAMERA_SENSOR_PROFILE");
    if (!profile_path || !*profile_path) profile_path = PROFILE;
    struct stat profile;
    if (stat(profile_path, &profile) || !S_ISREG(profile.st_mode) || profile.st_size < 1024) return -ENOENT;
    struct anyka_media_backend *b = calloc(1, sizeof *b);
    if (!b) return -ENOMEM;
    b->owned = malloc(CAMERA_RTSP_MAX_AU);
    if (!b->owned) { free(b); return -ENOMEM; }
    rc = anyka_sdk_bind_default(&b->api, error, capacity);
    if (rc) { free(b->owned); free(b); return rc; }
    *out = b;
    rc = checked(b, "exclusive-owner-recheck", anyka_trial_exclusive_owner()); if (rc) return rc;
    signal(SIGPIPE, SIG_IGN); signal(SIGHUP, SIG_IGN);
    b->hardware_attempted = 1;
#define SETUP(stage_name, call) do { rc = checked(b, stage_name, (call)); if (rc) return rc; } while (0)
    SETUP("mem-init-two-arguments", b->api.mem_init(0, 0));
    SETUP("VENC-pool-10MiB", b->api.dma_pool_create(ANYKA_DMA_MODULE_VENC, VENC_POOL));
    SETUP("VI-pool-13MiB", b->api.dma_pool_create(ANYKA_DMA_MODULE_VI, VI_POOL));
    #ifdef CAMERA_ANYKA_ENABLE_OSD
    SETUP("OSD-pool-512KiB", b->api.dma_pool_create(17u, OSD_POOL));
#endif
    SETUP("DMA-activate", b->api.dma_pool_activate());
    SETUP("VI-open", b->api.vi_open(0));
    SETUP("MIS2006-profile", b->api.vi_load_sensor_config(0, profile_path));
    struct anyka_sensor_timing timing;
    SETUP("sensor-timing-bind", anyka_sensor_timing_bind(&timing));
    uint32_t dimensions[2] = {0};
    SETUP("sensor-resolution", b->api.vi_get_sensor_resolution(0, dimensions));
    if (dimensions[0] != 1920 || dimensions[1] != 1080) return failed(b, "sensor-resolution-match", -ERANGE);
    struct anyka_vi_device_attr device = {0};
    device.crop_width = 1920; device.crop_height = 1080;
    device.main_max_width = 1920; device.main_max_height = 1080;
    device.fps = mode->fps; device.sub_max_width = 640; device.sub_max_height = 360;
    SETUP("VI-device-attributes", b->api.vi_set_device_attr(0, &device));
    struct anyka_vi_channel_attr main_channel = {0, mode->fps, mode->width, mode->height, 2};
    struct anyka_vi_channel_attr sub_channel = {1, mode->fps, 640, 360, 2};
    SETUP("VI-main-channel", b->api.vi_set_channel_attr(0, &main_channel));
    SETUP("VI-required-subchannel", b->api.vi_set_channel_attr(1, &sub_channel));
    SETUP("VI-device-enable", b->api.vi_enable_device(0));
    SETUP("VI-main-enable", b->api.vi_enable_channel(0));
    SETUP("sensor-timing-request", anyka_sensor_timing_apply(&timing,mode->fps));
    struct anyka_encode_param encode = {0};
    encode.width = mode->width; encode.height = mode->height; encode.fps = mode->fps; encode.gop_frames = 30;
    encode.target_kbps = 512; encode.maximum_kbps = 1024; encode.profile = mode->profile;
    encode.rate_control = 1; encode.initial_qp = 35; encode.minimum_qp = 25; encode.maximum_qp = 45;
    encode.chroma_mode = 1; encode.encoder_level = 50;
    int32_t handle = -1;
    SETUP("H264-main-encoder", b->api.venc_open(&encode, &handle));
    if (handle < 0 || handle > 7) return failed(b, "encoder-handle", -ERANGE);
    struct anyka_channel source = {ANYKA_DMA_MODULE_VI, 0};
    b->destination = (struct anyka_channel){ANYKA_DMA_MODULE_VENC, handle};
    struct anyka_channel_attr attr = {mode->fps, 2};
    SETUP("VI-to-VENC-bind", b->api.video_bind_channel(&source, &b->destination, &attr));
    SETUP("VENC-destination-active", b->api.video_set_destination_active(&b->destination, 1));
#undef SETUP
    uint64_t start = camera_monotonic_us();
    while (camera_monotonic_us() - start < UINT64_C(15000000)) {
        rc = pull(b);
        if (rc < 0) return b->fault;
        if (rc == 1 && b->keyframe && b->sps_bytes && b->pps_bytes) {
            memcpy(b->initial_sps,b->sps,b->sps_bytes);b->initial_sps_bytes=b->sps_bytes;
            memcpy(b->initial_pps,b->pps,b->pps_bytes);b->initial_pps_bytes=b->pps_bytes;
            b->first_pending = b->ready = 1;
            provider->opaque = b; provider->video = video;
            provider->description = (struct camera_media_description){b->initial_sps, b->initial_sps_bytes,
                b->initial_pps, b->initial_pps_bytes, CAMERA_AUDIO_NONE, 0};
            if (error && capacity) error[0] = 0;
            checked(b, "first-owned-IDR", 0); return 0;
        }
        usleep(10000);
    }
    return failed(b, "first-IDR-timeout", -ETIMEDOUT);
}

unsigned anyka_media_hardware_attempted(const struct anyka_media_backend *b)
{ return b ? b->hardware_attempted : 0; }
int anyka_media_error(const struct anyka_media_backend *b)
{ return b ? b->fault : -EINVAL; }
uint64_t anyka_media_delivered(const struct anyka_media_backend *b)
{ return b ? b->delivered : 0; }

int anyka_media_encoder_handle(const struct anyka_media_backend *b)
{return !b||b->fault||!b->ready?-ENODEV:b->destination.channel<0||b->destination.channel>7?-EPROTO:b->destination.channel;}

int anyka_media_open(struct anyka_media_backend **out,struct camera_media_provider *provider,char *error,size_t capacity)
{struct anyka_video_mode mode;anyka_video_mode_defaults(&mode);return anyka_media_open_mode(out,provider,&mode,error,capacity);}
