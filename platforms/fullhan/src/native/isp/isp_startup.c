#include "isp_startup.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static int fault(struct fh_isp_startup *s, enum fh_isp_stage stage, int rc)
{
    s->fault=1;s->uncertain_stage=stage;s->last_error=rc>0?-EIO:rc;
    return s->last_error;
}
static int owner(struct fh_isp_startup *s)
{
    if(!s || !s->capture || !s->platform || !s->platform->check)return -EINVAL;
    struct legacy_vmm *v=s->capture->vmm;
    if(s->fault || s->capture->fault || !v || !v->owned || v->fault ||
       v->quarantine || v->fd<0 || s->platform->quarantined)return -EPIPE;
    int rc=s->platform->check(s->platform->owner);
    return rc>0?-EIO:rc;
}
static uint32_t get(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int control(struct fh_isp_startup *s,enum fh_isp_stage stage,uint32_t cmd,
                    uint8_t *wire,size_t bytes)
{
    int rc=owner(s);
    if(rc)return fault(s,stage,rc);
    rc=s->io.ioctl(s->io.opaque,s->capture->isp_fd,cmd,wire,bytes);
    if(rc)return fault(s,stage,rc);
    s->last_completed=stage;
    return 0;
}
static int contained_user(const struct fh_capture_memory *m,uint32_t address,uint32_t bytes)
{
    return m->retained && address>=m->user && address-m->user<=m->size &&
        bytes<=m->size-(address-m->user);
}
/* Startup policy: retain the stock fixed values while deferring interrupt
 * delivery until the consumer is running. This ordering needs hardware
 * acceptance; the independently verified stock plan remains unchanged. */
static int masked_read(void *opaque,uint32_t offset,uint32_t *value)
{return fh_linux_io_isp_read(opaque,offset,value);}
static int masked_write(void *opaque,uint32_t offset,uint32_t value)
{return fh_linux_io_isp_write(opaque,offset,offset==8?0:value);}
int fh_isp_startup_init(struct fh_isp_startup *s,struct fh_capture *capture,
                        struct fh_linux_io *platform,const struct fh_isp_control_io *io)
{
    if(!s || !capture || !platform || !platform->check || !platform->quiesce ||
       !io || !io->ioctl || capture->isp_fd<0 || !capture->capacity_verified ||
       !capture->vpu_system_prepared || !capture->vpu_memory_prepared ||
       !capture->pae_system_prepared || !capture->vi_prepared || !capture->enable_ack)
        return -EINVAL;
    *s=(struct fh_isp_startup){.capture=capture,.platform=platform,.io=*io,
                              .allocation=(size_t)-1};
    return owner(s);
}
static int native_format(const struct mis2006_stock_format *format)
{
    return format && format->sensor_width==1928 && format->sensor_height==1088 &&
        format->crop_width==1920 && format->crop_height==1080 &&
        format->crop_x==4 && format->crop_y==4 && format->field_14==1;
}
int fh_isp_startup_prepare_memory(struct fh_isp_startup *s)
{
    int rc=owner(s);
    if(rc)return rc;
    if(s->prepared || s->retained || s->last_completed!=FH_ISP_NEW)return -EALREADY;
    uint8_t query[32]={0};
    rc=control(s,FH_ISP_QUERY,FH_ISP_QUERY_NR3D,query,sizeof query);
    if(rc)return rc;
    for(unsigned i=0;i<8;i++)s->capabilities[i]=get(query+4*i);
    if(s->capabilities[0]>1)return fault(s,FH_ISP_QUERY,-ENOTSUP);
    uint32_t need,backing;
    rc=fh_isp_memory_size(s->capabilities[0],&need,&backing);
    if(rc)return fault(s,FH_ISP_ALLOCATE,rc);
    (void)backing;
    struct legacy_vmm *v=s->capture->vmm;
    rc=owner(s);
    if(rc)return fault(s,FH_ISP_ALLOCATE,rc);
    rc=legacy_vmm_allocate_isp(v,need,&s->allocation);
    if(s->allocation!=(size_t)-1)s->retained=1;
    if(rc)return fault(s,FH_ISP_ALLOCATE,rc);
    s->last_completed=FH_ISP_ALLOCATE;
    rc=owner(s);
    if(rc)return fault(s,FH_ISP_MAP,rc);
    rc=legacy_vmm_map(v,s->allocation,s->capture->map_method);
    if(rc)return fault(s,FH_ISP_MAP,rc);
    s->last_completed=FH_ISP_MAP;
    const struct legacy_vmm_block *block=&v->blocks[s->allocation];
    rc=fh_isp_memory_bind(s->capabilities[0],block->phys,block->user,block->size,&s->memory);
    if(rc)return fault(s,FH_ISP_BIND,rc);
    s->last_completed=FH_ISP_BIND;
    return 0;
}
int fh_isp_startup_prepare_registers(struct fh_isp_startup *s,
                                   const struct mis2006_stock_format *format)
{
    int rc=owner(s);
    if(rc)return rc;
    if(s->prepared)return -EALREADY;
    if(!s->retained || s->last_completed!=FH_ISP_BIND)return -EPERM;
    if(!native_format(format))return -EINVAL;
    uint8_t mode[4]={0},addresses[FH_ISP_ADDRESS_REQUEST_BYTES],package[4]={0};
    rc=fh_isp_memory_address_request(&s->memory,addresses);
    if(rc)return fault(s,FH_ISP_ADDRESSES,rc);
    if((rc=control(s,FH_ISP_MODE,FH_ISP_BUFFER_MODE,mode,sizeof mode)) ||
       (rc=control(s,FH_ISP_ADDRESSES,FH_ISP_ADDRESS,addresses,sizeof addresses)) ||
       (rc=control(s,FH_ISP_PACKAGE,FH_ISP_VPU_PACKAGE,package,sizeof package)))return rc;
    /* Matching vpu_sys_mem_init places media_malloc's {phys,user,size} at
     * vpu+0x3c. ISP ioctl returns +0x40: USER address, not physical +0x3c. */
    s->package_user=get(package);
    if(!contained_user(&s->capture->vpu_sys,s->package_user,0x800))
        return fault(s,FH_ISP_PACKAGE,-EPROTO);
    if(!s->platform->isp.mapped) {
        rc=fh_linux_io_map_isp(s->platform);
        if(rc)return fault(s,FH_ISP_MMIO,rc);
    }
    s->last_completed=FH_ISP_MMIO;
    struct fh_isp_fixed_io fixed={s->platform,masked_read,masked_write};
    rc=fh_linux_io_isp_write(s->platform,8,0);
    if(!rc)rc=fh_isp_fixed_run(&fixed,&s->fixed);
    if(rc)return fault(s,FH_ISP_FIXED,rc);
    s->last_completed=FH_ISP_FIXED;
    rc=fh_linux_io_isp_write(s->platform,8,0);
    if(rc)return fault(s,FH_ISP_MASKED,rc);
    s->last_completed=FH_ISP_MASKED;
    struct fh_isp_geometry_io geometry;
    rc=fh_linux_io_geometry_ops(s->platform,&geometry);
    if(!rc)rc=fh_isp_geometry_run(format,&geometry,&s->geometry);
    if(rc)return fault(s,FH_ISP_GEOMETRY,rc);
    s->last_completed=FH_ISP_PREPARED;s->prepared=1;
    return 0;
}
int fh_isp_startup_prepare(struct fh_isp_startup *s,const struct mis2006_stock_format *format)
{
    if(!native_format(format))return -EINVAL;
    int rc=fh_isp_startup_prepare_memory(s);
    return rc?rc:fh_isp_startup_prepare_registers(s,format);
}
int fh_isp_startup_enable(struct fh_isp_startup *s)
{
    int rc=owner(s);
    if(rc)return rc;
    if(!s->prepared || !s->capture->bound_ack || !s->capture->encoder_prepared ||
       !s->capture->start_ack || s->capture->stop_attempted || s->capture->stop_ack)
        return -EPERM;
    if(s->producer_attempted)return -EALREADY;
    s->producer_attempted=1;
    /* Observed 104588 mode path resets counters/once after geometry and before
     * 10620c producer start. This is not proof of the full cold-init sequence. */
    if((rc=control(s,FH_ISP_COUNTERS,FH_ISP_RESET_COUNTERS,NULL,0)) ||
       (rc=control(s,FH_ISP_ONCE,FH_ISP_ONCE_INIT,NULL,0)))return rc;
    uint32_t value;
    rc=fh_linux_io_isp_write(s->platform,0x18,1);
    if(!rc)rc=fh_linux_io_isp_read(s->platform,0x68,&value);
    if(!rc)rc=fh_linux_io_isp_write(s->platform,0x68,value|1);
    if(!rc)rc=fh_linux_io_isp_read(s->platform,4,&value);
    if(!rc && value)rc=fh_linux_io_isp_write(s->platform,4,value);
    /* Exact stock fixed init's mask. This write ACK is not frame acceptance. */
    if(!rc)rc=fh_linux_io_isp_write(s->platform,8,UINT32_MAX);
    if(rc)return fault(s,FH_ISP_ENABLE,rc);
    s->producer_ack=1;s->last_completed=FH_ISP_ENABLE;
    return 0;
}
int fh_isp_control_linux_ioctl(void *opaque,int fd,uint32_t cmd,uint8_t *wire,size_t bytes)
{
    (void)opaque;
    size_t expected;
    switch(cmd) {
    case UINT32_C(0x40046924):expected=652;break; /* gamma includes pending word */
    case FH_ISP_QUERY_NR3D:expected=32;break;
    case FH_ISP_BUFFER_MODE:case FH_ISP_VPU_PACKAGE:expected=4;break;
    case FH_ISP_ADDRESS:expected=92;break;
    case FH_ISP_RESET_COUNTERS:case FH_ISP_ONCE_INIT:expected=0;break;
    default:return -ENOTSUP;
    }
    if(fd<0 || bytes!=expected || (bytes && !wire))return -EINVAL;
    int rc=ioctl(fd,(unsigned long)cmd,wire);
    return rc==-1?-errno:rc;
}
