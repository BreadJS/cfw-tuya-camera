#include "fh_capture.h"
#include <errno.h>
#include <string.h>

static void put(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int failed(struct fh_capture *s,enum fh_capture_stage stage,int rc)
{
    s->fault=1; s->uncertain_stage=stage; s->last_error=rc>0 ? -EIO : rc;
    return s->last_error;
}
static int ready(struct fh_capture *s)
{
    if (!s) return -EINVAL;
    if (!s->vmm || s->fault || !s->vmm->owned || s->vmm->fault || s->vmm->fd<0) return -EPIPE;
    return 0;
}
static int call(struct fh_capture *s,enum fh_capture_stage stage,int fd,uint32_t cmd,
                 uint32_t *words,size_t count)
{
    uint8_t wire[84];
    size_t i;
    int rc;
    if (count>21) return failed(s,stage,-EINVAL);
    for (i=0;i<count;++i) put(wire+4*i,words[i]);
    s->last_command=cmd;
    rc=s->io.ioctl(s->io.opaque,fd,cmd,wire,count*4);
    s->last_ioctl_result=rc;
    if (rc) return failed(s,stage,rc);
    for (i=0;i<count;++i) words[i]=get(wire+4*i);
    s->last_completed=stage;
    return 0;
}
static int memory(struct fh_capture *s,struct fh_capture_memory *m,const char *name,
                   uint32_t need,enum fh_capture_stage alloc,enum fh_capture_stage map)
{
    size_t handle=(size_t)-1;
    int rc;
    if (!need || need>LEGACY_VMM_ZONE_END-LEGACY_VMM_ZONE_START)
        return failed(s,alloc,-EPROTO);
    rc=legacy_vmm_allocate(s->vmm,name,need,4096,&handle);
    if (handle!=(size_t)-1) { m->handle=handle; m->retained=1; }
    if (rc) return failed(s,alloc,rc);
    s->last_completed=alloc;
    m->phys=s->vmm->blocks[handle].phys;
    m->size=s->vmm->blocks[handle].size;
    rc=legacy_vmm_map(s->vmm,handle,s->map_method);
    if (rc) return failed(s,map,rc);
    m->user=s->vmm->blocks[handle].user;
    s->last_completed=map;
    return 0;
}
void fh_capture_default_config(struct fh_capture_config *c)
{
    if (!c) return;
    c->channel=0; c->fps=15; c->gop=30; c->profile=77;
    c->qp=c->i_qp=c->p_qp=28;
    c->experimental_fps30=0;
    c->output_width=1920;c->output_height=1080;
}
int fh_capture_validate_config(const struct fh_capture_config *c)
{
    if (!c) return -EINVAL;
    if (c->channel) return -ENOTSUP;
    if((c->output_width!=1920 || c->output_height!=1080) &&
       !(c->experimental_fps30 && c->output_width==1280 && c->output_height==720))return -ERANGE;
    if (c->experimental_fps30>1 || !c->fps ||
        (c->fps>15 && !(c->experimental_fps30 && c->fps==30)) || !c->gop || c->gop>65535 ||
        (c->profile!=66 && c->profile!=77) || c->qp>51 || c->i_qp>51 || c->p_qp>51)
        return -ERANGE;
    return 0;
}
int fh_capture_init(struct fh_capture *s,const struct fh_capture_config *c,
                    const struct fh_capture_io *io,struct legacy_vmm *vmm,
                    int isp,int pae,int media,enum legacy_vmm_map_method method)
{
    int rc=fh_capture_validate_config(c);
    if (rc) return rc;
    if (!s || !io || !io->ioctl || !vmm || isp<0 || pae<0 || media<0 ||
        (method!=LEGACY_VMM_MAP_IOCTL && method!=LEGACY_VMM_MAP_DIRECT)) return -EINVAL;
    if (!vmm->owned || vmm->fault || vmm->fd<0) return -EPERM;
    memset(s,0,sizeof(*s));
    s->config=*c; s->io=*io; s->vmm=vmm;
    s->isp_fd=isp; s->pae_fd=pae; s->media_fd=media; s->map_method=method;
    return 0;
}
#define CALL(stage,fd,cmd,n) do { rc=call(s,stage,fd,cmd,w,n); if (rc) return rc; } while(0)
int fh_capture_prepare_vpu_system(struct fh_capture *s)
{
    uint32_t w[3]={0};
    int rc=ready(s);
    if (rc) return rc;
    if (!s->capacity_verified) return -EPERM;
    if (s->vpu_system_prepared) return -EALREADY;
    CALL(FH_CAP_VPU_SYS_QUERY_STAGE,s->isp_fd,FH_CAP_VPU_SYS_QUERY,1);
    rc=memory(s,&s->vpu_sys,"vpu_sys",w[0],FH_CAP_VPU_SYS_ALLOC,FH_CAP_VPU_SYS_MAP);
    if (rc) return rc;
    w[0]=s->vpu_sys.phys; w[1]=s->vpu_sys.user; w[2]=s->vpu_sys.size;
    CALL(FH_CAP_VPU_SYS_INIT_STAGE,s->isp_fd,FH_CAP_VPU_SYS_INIT,3);
    s->vpu_system_prepared=1;return 0;
}
int fh_capture_prepare_vi(struct fh_capture *s)
{
    uint32_t w[3];int rc=ready(s);
    if (rc) return rc;
    if (!s->capacity_verified || !s->vpu_system_prepared || s->vi_prepared) return -EPIPE;
    w[0]=1920; w[1]=1080; w[2]=0;
    CALL(FH_CAP_VI_STAGE,s->isp_fd,FH_CAP_VPU_VI,3);
    s->vi_prepared=1;return 0;
}
int fh_capture_prepare_vpu_channel_memory(struct fh_capture *s)
{
    uint32_t w[6]={0},ch;int rc=ready(s);
    if (rc) return rc;
    if (!s->capacity_verified || !s->vpu_system_prepared || s->vpu_memory_prepared) return -EPIPE;
    ch=s->config.channel;
    w[0]=ch; w[1]=1920; w[2]=1080; w[3]=0;
    CALL(FH_CAP_VPU_QUERY_STAGE,s->isp_fd,FH_CAP_VPU_QUERY,4);
    if (w[0]!=ch || w[1]!=1920 || w[2]!=1080) return failed(s,FH_CAP_VPU_QUERY_STAGE,-EPROTO);
    rc=memory(s,&s->vpu_channel,"vpu_chn0",w[3],FH_CAP_VPU_ALLOC,FH_CAP_VPU_MAP);
    if (rc) return rc;
    w[0]=ch; w[1]=s->vpu_channel.phys; w[2]=s->vpu_channel.user;
    w[3]=s->vpu_channel.size; w[4]=1920; w[5]=1080;
    CALL(FH_CAP_VPU_MEMORY_STAGE,s->isp_fd,FH_CAP_VPU_MEMORY,6);
    s->vpu_memory_prepared=1;return 0;
}
int fh_capture_finish_vpu(struct fh_capture *s)
{
    uint32_t w[3],ch,fps;int rc=ready(s);
    if (rc) return rc;
    if (!s->vi_prepared || !s->vpu_memory_prepared || s->vpu_prepared) return -EPIPE;
    ch=s->config.channel;fps=0x10000u|s->config.fps;
    w[0]=ch; w[1]=s->config.output_width; w[2]=s->config.output_height;
    CALL(FH_CAP_VPU_CONFIG_STAGE,s->isp_fd,FH_CAP_VPU_CONFIG,3);
    w[0]=ch;
    CALL(FH_CAP_VPU_OPEN_STAGE,s->isp_fd,FH_CAP_VPU_OPEN,1);
    w[0]=ch; w[1]=fps;
    CALL(FH_CAP_PACE_SET_STAGE,s->isp_fd,FH_CAP_VPU_PACE_SET,2);
    w[0]=ch; w[1]=0;
    CALL(FH_CAP_PACE_GET_STAGE,s->isp_fd,FH_CAP_VPU_PACE_GET,2);
    if (w[0]!=ch || w[1]!=fps) return failed(s,FH_CAP_PACE_GET_STAGE,-EPROTO);
    s->vpu_prepared=1;
    return 0;
}
int fh_capture_prepare_vpu(struct fh_capture *s)
{
    int rc=ready(s);
    if (rc) return rc;
    if (!s->capacity_verified) return -EPERM;
    if (s->vpu_prepared || s->vpu_system_prepared) return -EALREADY;
    rc=fh_capture_prepare_vpu_system(s);
    if (!rc) rc=fh_capture_prepare_vi(s);
    if (!rc) rc=fh_capture_prepare_vpu_channel_memory(s);
    if (!rc) rc=fh_capture_finish_vpu(s);
    return rc;
}
int fh_capture_enable_vpu(struct fh_capture *s)
{
    uint32_t ch;
    int rc=ready(s);
    if (rc) return rc;
    if (!s->vi_prepared || !s->vpu_memory_prepared || s->enable_ack || s->encoder_prepared) return -EPIPE;
    ch=s->config.channel;
    rc=call(s,FH_CAP_VPU_ENABLE_STAGE,s->isp_fd,FH_CAP_VPU_ENABLE,&ch,1);
    if (!rc) s->enable_ack=1;
    return rc;
}
int fh_capture_prepare_encoder_system(struct fh_capture *s)
{
    uint32_t w[3]={0};
    int rc=ready(s);
    if (rc) return rc;
    if (!s->capacity_verified || !s->vpu_memory_prepared || s->pae_system_prepared) return -EPIPE;
    CALL(FH_CAP_PAE_SYS_QUERY_STAGE,s->pae_fd,FH_CAP_PAE_SYS_QUERY,1);
    rc=memory(s,&s->pae_sys,"pae_sys",w[0],FH_CAP_PAE_SYS_ALLOC,FH_CAP_PAE_SYS_MAP);
    if (rc) return rc;
    w[0]=s->pae_sys.phys; w[1]=s->pae_sys.user; w[2]=s->pae_sys.size;
    CALL(FH_CAP_PAE_SYS_INIT_STAGE,s->pae_fd,FH_CAP_PAE_SYS_INIT,3);
    s->pae_system_prepared=1;return 0;
}
int fh_capture_prepare_encoder(struct fh_capture *s)
{
    uint32_t w[21]={0},ch,fps;
    int rc=ready(s);
    if (rc) return rc;
    if (!s->vpu_prepared || !s->enable_ack || s->encoder_prepared) return -EPIPE;
    if (!s->pae_system_prepared) {
        rc=fh_capture_prepare_encoder_system(s);
        if (rc) return rc;
    }
    ch=s->config.channel;fps=0x10000u|s->config.fps;
    w[0]=ch; w[1]=0; w[2]=1920; w[3]=1080; w[4]=0;
    CALL(FH_CAP_PAE_QUERY_STAGE,s->pae_fd,FH_CAP_PAE_QUERY,5);
    if (w[0]!=ch || w[2]!=1920 || w[3]!=1080 || w[4]) return failed(s,FH_CAP_PAE_QUERY_STAGE,-EPROTO);
    rc=memory(s,&s->pae_channel,"pae_enc0",w[1],FH_CAP_PAE_ALLOC,FH_CAP_PAE_MAP);
    if (rc) return rc;
    w[0]=ch; w[1]=s->pae_channel.phys; w[2]=s->pae_channel.user;
    w[3]=s->pae_channel.size; w[4]=1920; w[5]=1080; w[6]=0;
    CALL(FH_CAP_PAE_MEMORY_STAGE,s->pae_fd,FH_CAP_PAE_MEMORY,7);
    memset(w,0,sizeof(w));
    w[0]=ch; w[1]=s->config.output_width; w[2]=s->config.output_height; w[3]=s->config.gop;
    w[4]=s->config.profile; w[5]=s->config.qp; w[6]=fps;
    CALL(FH_CAP_PAE_CONFIG_STAGE,s->pae_fd,FH_CAP_PAE_CONFIG,11);
    memset(w,0,sizeof(w));
    w[0]=ch; w[1]=2; w[2]=fps; w[3]=s->config.i_qp; w[4]=s->config.p_qp;
    CALL(FH_CAP_RC_STAGE,s->pae_fd,FH_CAP_PAE_RC,21);
    s->encoder_prepared=1;
    return 0;
}
#undef CALL
int fh_capture_force_idr(struct fh_capture *s)
{
    uint32_t ch;
    int rc=ready(s);
    if (rc) return rc;
    if (!s->start_ack || s->stop_ack) return -EPIPE;
    ch=s->config.channel;
    rc=call(s,FH_CAP_IDR_STAGE,s->pae_fd,FH_CAP_PAE_IDR,&ch,1);
    if (!rc) s->idr_ack=1;
    return rc;
}
int fh_capture_start_routing(struct fh_capture *s)
{
    uint32_t w[2];
    int rc=ready(s);
    if (rc) return rc;
    if (!s->encoder_prepared || s->start_ack || s->bound_ack) return -EPIPE;
    w[0]=s->config.channel;
    s->start_attempted=1;
    rc=call(s,FH_CAP_START_STAGE,s->pae_fd,FH_CAP_PAE_START,w,1);
    if (rc) return rc;
    s->start_ack=1;
    rc=fh_capture_force_idr(s);
    if (rc) return rc;
    w[0]=1; w[1]=7;
    s->bind_attempted=1;
    rc=call(s,FH_CAP_BIND_STAGE,s->media_fd,FH_CAP_MEDIA_BIND,w,2);
    if (!rc) s->bound_ack=1;
    return rc;
}
int fh_capture_request_encoder_stop(struct fh_capture *s)
{
    uint32_t ch;
    int rc;
    if (!s || !s->vmm || !s->vmm->owned || s->vmm->fd<0) return -EPERM;
    if (!s->start_attempted || s->stop_ack) return -EPIPE;
    ch=s->config.channel;
    s->stop_attempted=1;
    rc=call(s,FH_CAP_STOP_REQUEST_STAGE,s->pae_fd,FH_CAP_PAE_STOP,&ch,1);
    if (!rc) s->stop_ack=1;
    return rc;
}
