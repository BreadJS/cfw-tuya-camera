#include "fh_video_provider.h"
#include <errno.h>
#include <string.h>

static int failed(struct fh_video_provider *p,int rc)
{ p->faulted=1;p->last_error=rc>0?-EIO:rc;return -1; }
static int parameter(const struct fh_frame *f,struct fh_frame_nal span,unsigned kind)
{
    if(!span.bytes || span.bytes>CAMERA_RTSP_MAX_PARAMETER_SET || span.offset>=f->bytes ||
       span.bytes>f->bytes-span.offset)return -EPROTO;
    return (f->data[span.offset]&0x9f)==kind ? 0:-EPROTO;
}
static size_t prefix(const uint8_t *data,size_t bytes,size_t from,size_t *length)
{
    for(size_t i=from;i<bytes && bytes-i>=3;i++) {
        if(data[i] || data[i+1])continue;
        if(bytes-i>=4 && !data[i+2] && data[i+3]==1) { *length=4;return i; }
        if(data[i+2]==1) { *length=3;return i; }
    }
    return bytes;
}
/* Inspect ALL parameter NALs, not only fh_frame's first-SPS/PPS shortcuts.
 * Repeated same sets are legal; even a later changed set must force restart. */
static int same_parameters(const struct fh_video_provider *p,const struct fh_frame *f)
{
    size_t start_length=0,next_length=0;
    size_t start=prefix(f->data,f->bytes,0,&start_length);
    if(start==f->bytes)return -EBADMSG;
    for(size_t i=0;i<start;i++)if(f->data[i])return -EBADMSG;
    while(start<f->bytes) {
        size_t header=start+start_length;
        if(header==f->bytes || (f->data[header]&0x80))return -EBADMSG;
        unsigned kind=f->data[header]&31;
        if(!kind)return -EBADMSG;
        size_t next=prefix(f->data,f->bytes,header+1,&next_length);
        const uint8_t *expected=kind==7?p->sps:p->pps;
        size_t expected_bytes=kind==7?p->sps_bytes:p->pps_bytes;
        if((kind==7 || kind==8) &&
           (next-header!=expected_bytes || memcmp(f->data+header,expected,expected_bytes)))return -EPROTO;
        start=next;start_length=next_length;
    }
    return 0;
}
static int frame_valid(const struct fh_video_provider *p,const struct fh_frame *f)
{
    if(!f->data || f->data!=p->reader->buffer || !f->bytes ||
       f->bytes>CAMERA_RTSP_MAX_AU || f->bytes>p->reader->capacity)return -EMSGSIZE;
    if(!f->timestamp_ns_valid)return -ENODATA;
    if(p->reader->queue_owned || p->reader->ownership_uncertain || p->reader->busy || p->reader->quarantine)
        return -ENOTRECOVERABLE;
    return same_parameters(p,f);
}
int fh_video_provider_init(struct fh_video_provider *p,struct fh_frame_reader *reader,
                            const struct fh_frame *first,struct camera_media_provider *provider)
{
    if(provider)memset(provider,0,sizeof *provider);
    if(!p || !reader || !first || !provider)return -EINVAL;
    memset(p,0,sizeof *p);p->reader=reader;
    if(!reader->clock.raw_to_ns)return -ENOTSUP;
    if(!reader->capture || !reader->buffer || !reader->capacity ||
       first->data!=reader->buffer || !first->bytes || first->bytes>reader->capacity ||
       first->bytes>CAMERA_RTSP_MAX_AU)return -EMSGSIZE;
    if(!first->timestamp_ns_valid)return -ENODATA;
    if(!first->has_sps || !first->has_pps)return -ENODATA;
    int rc=parameter(first,first->sps,7);if(rc)return rc;
    rc=parameter(first,first->pps,8);if(rc)return rc;
    p->sps_bytes=first->sps.bytes;p->pps_bytes=first->pps.bytes;
    memcpy(p->sps,first->data+first->sps.offset,p->sps_bytes);
    memcpy(p->pps,first->data+first->pps.offset,p->pps_bytes);
    rc=frame_valid(p,first);if(rc)return rc;
    p->clock=reader->clock;p->first=*first;p->first_pending=p->initialized=1;
    provider->opaque=p;
    provider->description=(struct camera_media_description){p->sps,p->sps_bytes,p->pps,p->pps_bytes,CAMERA_AUDIO_NONE,0};
    provider->video=fh_video_provider_video;
    return 0;
}
int fh_video_provider_video(void *opaque,uint64_t now_us,struct camera_media_frame *out)
{
    struct fh_video_provider *p=opaque;(void)now_us;
    if(out)memset(out,0,sizeof *out);
    if(!p || !p->initialized)return -1;
    if(!out)return failed(p,-EINVAL);
    if(p->faulted)return -1;
    if(!p->reader || p->reader->clock.raw_to_ns!=p->clock.raw_to_ns ||
       p->reader->clock.opaque!=p->clock.opaque)return failed(p,-ESTALE);
    struct fh_frame frame;
    if(p->first_pending) { frame=p->first;p->first_pending=0; }
    else {
        int rc=fh_frame_read(p->reader,&frame);
        if(rc==FH_FRAME_EMPTY)return 0;
        if(rc)return failed(p,rc);
    }
    int rc=frame_valid(p,&frame);if(rc)return failed(p,rc);
    uint64_t us=frame.timestamp_ns/UINT64_C(1000);
    if(p->delivered && us<=p->last_us)return failed(p,-ERANGE);
    p->last_us=us;p->delivered=1;p->last_error=0;
    *out=(struct camera_media_frame){.data=frame.data,.size=frame.bytes,.pts_us=us,.keyframe=frame.has_idr?1:0};
    return 1;
}
