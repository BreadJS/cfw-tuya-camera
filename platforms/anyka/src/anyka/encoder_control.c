/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "encoder_control.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
struct encoder_sdk {
    const char *(*version)(void);
    int (*get)(unsigned,struct anyka_encode_param *);
    int (*set)(unsigned,const struct anyka_encode_param *);
    int (*qp)(unsigned,int,int);
    int (*iframe_get)(unsigned,int32_t *,int32_t *,int32_t *,int32_t *);
    int (*iframe_set)(unsigned,int,int,int,int);
    int (*idr)(unsigned);
};
struct anyka_encoder_control {
    struct encoder_sdk sdk;
    struct anyka_encoder_snapshot state;
    unsigned handle;
};
static int bind(struct encoder_sdk *s)
{
    memset(s,0,sizeof *s);void *address;
#define BIND(member,name) do{address=dlsym(RTLD_DEFAULT,name);if(!address||sizeof s->member!=sizeof address)return -ENOSYS;memcpy(&s->member,&address,sizeof address);}while(0)
    BIND(version,"ak_venc_get_version");BIND(get,"ak_venc_get_attr");BIND(set,"ak_venc_set_attr");
    BIND(qp,"ak_venc_set_qp");BIND(iframe_get,"ak_venc_get_iframe_param");
    BIND(iframe_set,"ak_venc_set_iframe_param");BIND(idr,"ak_venc_request_idr");
#undef BIND
    return 0;
}
static int finish(struct anyka_encoder_control *c,int error,int sdk,struct anyka_encoder_snapshot *out)
{c->state.error=error;c->state.sdk_result=sdk;if(out)*out=c->state;return error;}
static void settings(const struct anyka_encode_param *a,struct anyka_encoder_settings *out)
{
    *out=(struct anyka_encoder_settings){a->target_kbps,a->maximum_kbps,a->gop_frames,
        a->initial_qp,a->minimum_qp,a->maximum_qp,(unsigned)a->rate_control};
}
static int valid(const struct anyka_encode_param *a)
{
    /* Recovered target converter0xc29f8. This adapter selects H264 only. */
    return a->codec==0&&a->profile>=0&&a->profile<=3&&a->width>=8&&a->width<=2592&&
        !(a->width&7)&&a->height>=8&&a->height<=2592&&!(a->height&7)&&a->fps&&
        a->gop_frames>=1&&a->gop_frames<=1000&&a->target_kbps&&a->maximum_kbps>=a->target_kbps&&
        a->rate_control>=0&&a->rate_control<=1&&a->minimum_qp<=a->initial_qp&&
        a->initial_qp<=a->maximum_qp&&a->maximum_qp<=51&&a->chroma_mode>=0&&a->chroma_mode<=2&&
        a->jpeg_quality>=0&&a->jpeg_quality<=3&&a->encoder_level>=10&&a->encoder_level<=50&&
        a->smart_mode>=0&&a->smart_mode<=3&&a->smart_gop_frames<=1000&&
        a->smart_quality<=100&&a->smart_static_value<=768;
}
int anyka_encoder_control_read(struct anyka_encoder_control *c,struct anyka_encoder_snapshot *out)
{
    if(!c)return -EINVAL;
    struct anyka_encode_param a={0};int rc=c->sdk.get(c->handle,&a);
    if(rc)return finish(c,-EIO,rc,out);
    if(!valid(&a))return finish(c,-EPROTO,0,out);
    c->state.attributes=a;settings(&a,&c->state.settings);
    /* Exact inner getter writes only halfwords to the first two pointers.
     * Zeroed32-bit words are safe for that ABI and its int* public declaration. */
    int32_t low=0,high=0,min_size=0,max_size=0;
    rc=c->sdk.iframe_get(c->handle,&low,&high,&min_size,&max_size);
    c->state.iframe_valid=!rc&&low>=0&&high>=low&&high<=51&&min_size>=0&&max_size>=min_size;
    if(c->state.iframe_valid)c->state.iframe_effective=(struct anyka_encoder_iframe){(unsigned)low,(unsigned)high,(unsigned)min_size,(unsigned)max_size};
    else memset(&c->state.iframe_effective,0,sizeof c->state.iframe_effective);
    /* A derived I-frame size can be invalid at extreme SDK-legal bitrates.
     * Keep ordinary controls usable so a caller can repair that configuration. */
    return finish(c,0,rc,out);
}
static int open_bound(struct anyka_encoder_control **out,unsigned handle,const struct encoder_sdk *sdk)
{
    if(!out||*out||handle>7||!sdk||!sdk->version||!sdk->get||!sdk->set||!sdk->qp||
       !sdk->iframe_get||!sdk->iframe_set||!sdk->idr)return -EINVAL;
    const char *version=sdk->version();if(!version||strcmp(version,"libmpi_venc V1.0.29"))return -EPROTO;
    struct anyka_encoder_control *c=calloc(1,sizeof *c);if(!c)return -ENOMEM;
    c->sdk=*sdk;c->handle=handle;int rc=anyka_encoder_control_read(c,NULL);
    if(rc){free(c);return rc;}*out=c;return 0;
}
int anyka_encoder_control_open(struct anyka_encoder_control **out,unsigned handle)
{
    if(!out||*out||handle>7)return -EINVAL;
    struct encoder_sdk sdk;int rc=bind(&sdk);return rc?rc:open_bound(out,handle,&sdk);
}
int anyka_encoder_control_idr(struct anyka_encoder_control *c)
{
    if(!c)return -EINVAL;
    int rc=c->sdk.idr(c->handle);if(rc)return finish(c,-EIO,rc,NULL);
    c->state.idr_requests++;return finish(c,0,0,NULL);
}
int anyka_encoder_request_idr(struct anyka_encoder_control *c)
{return anyka_encoder_control_idr(c);}
int anyka_encoder_control_apply(struct anyka_encoder_control *c,unsigned fields,
    const struct anyka_encoder_settings *requested,struct anyka_encoder_snapshot *out)
{
    if(!c||!requested||(fields&~ANYKA_ENCODER_ALL))return -EINVAL;
    int rc=anyka_encoder_control_read(c,NULL);if(rc){if(out)*out=c->state;return rc;}
    struct anyka_encode_param proposed=c->state.attributes;
    /* Check wide public inputs BEFORE narrowing them into the exact ABI. */
    if((fields&ANYKA_ENCODER_BITRATE)&&(!requested->target_kbps||requested->target_kbps>65535||requested->maximum_kbps>65535||requested->maximum_kbps<requested->target_kbps))return finish(c,-ERANGE,0,out);
    if((fields&ANYKA_ENCODER_GOP)&&(!requested->gop_frames||requested->gop_frames>1000))return finish(c,-ERANGE,0,out);
    if((fields&ANYKA_ENCODER_QP)&&(requested->minimum_qp>requested->initial_qp||requested->initial_qp>requested->maximum_qp||requested->maximum_qp>51))return finish(c,-ERANGE,0,out);
    if((fields&ANYKA_ENCODER_RATE_CONTROL)&&requested->rate_control>1)return finish(c,-ERANGE,0,out);
    if(fields&ANYKA_ENCODER_BITRATE){proposed.target_kbps=(uint16_t)requested->target_kbps;proposed.maximum_kbps=(uint16_t)requested->maximum_kbps;}
    if(fields&ANYKA_ENCODER_GOP)proposed.gop_frames=(uint16_t)requested->gop_frames;
    if(fields&ANYKA_ENCODER_QP){proposed.initial_qp=(uint16_t)requested->initial_qp;proposed.minimum_qp=(uint16_t)requested->minimum_qp;proposed.maximum_qp=(uint16_t)requested->maximum_qp;}
    if(fields&ANYKA_ENCODER_RATE_CONTROL)proposed.rate_control=(int32_t)requested->rate_control;
    if(!valid(&proposed))return finish(c,-ERANGE,0,out);
    if(!memcmp(&proposed,&c->state.attributes,sizeof proposed))return finish(c,0,0,out);
    rc=c->sdk.set(c->handle,&proposed);if(rc)return finish(c,-EIO,rc,out);
    rc=anyka_encoder_control_read(c,NULL);if(rc){if(out)*out=c->state;return rc;}
    if(memcmp(&proposed,&c->state.attributes,sizeof proposed))return finish(c,-EIO,0,out);
    c->state.changes++;rc=anyka_encoder_control_idr(c);
    if(out)*out=c->state;
    return rc;
}
int anyka_encoder_control_iframe(struct anyka_encoder_control *c,
    const struct anyka_encoder_iframe *r,struct anyka_encoder_snapshot *out)
{
    if(!c||!r)return -EINVAL;
    int rc=anyka_encoder_control_read(c,NULL);if(rc){if(out)*out=c->state;return rc;}
    int automatic_qp=!r->minimum_qp&&!r->maximum_qp;
    int automatic_size=!r->minimum_kbytes&&!r->maximum_kbytes;
    if(!automatic_qp&&(r->minimum_qp<c->state.attributes.minimum_qp||r->maximum_qp>c->state.attributes.maximum_qp||
       !r->minimum_qp||r->minimum_qp>r->maximum_qp))return finish(c,-ERANGE,0,out);
    if(!automatic_size&&(!r->minimum_kbytes||r->maximum_kbytes>ANYKA_ENCODER_IFRAME_MAX_KBYTES||
       r->maximum_kbytes<2||r->minimum_kbytes<r->maximum_kbytes/5||r->minimum_kbytes>r->maximum_kbytes/2))return finish(c,-ERANGE,0,out);
    rc=c->sdk.iframe_set(c->handle,(int)r->minimum_qp,(int)r->maximum_qp,(int)r->minimum_kbytes,(int)r->maximum_kbytes);
    if(rc)return finish(c,-EIO,rc,out);
    c->state.iframe_requested=*r;c->state.iframe_request_known=1;
    rc=anyka_encoder_control_read(c,NULL);if(rc){if(out)*out=c->state;return rc;}
    if(!c->state.iframe_valid)return finish(c,-EIO,0,out);
    c->state.iframe_adjusted=(!automatic_qp&&(r->minimum_qp!=c->state.iframe_effective.minimum_qp||r->maximum_qp!=c->state.iframe_effective.maximum_qp))||
        (!automatic_size&&(r->minimum_kbytes!=c->state.iframe_effective.minimum_kbytes||r->maximum_kbytes!=c->state.iframe_effective.maximum_kbytes));
    c->state.changes++;rc=anyka_encoder_control_idr(c);if(out)*out=c->state;return rc;
}
void anyka_encoder_control_free(struct anyka_encoder_control *c){free(c);}
