#include "runtime.h"
#include <errno.h>
#include <string.h>
static int checked(int rc) { return rc>0?-EIO:rc; }
static int owned(struct camera_osd *s)
{
    if (!s || !s->initialized || !s->io.check_owner || !s->io.submit) return -EINVAL;
    return checked(s->io.check_owner(s->io.opaque));
}
int camera_osd_init(struct camera_osd *s,int isp_fd,struct legacy_vmm *vmm,
                    const struct camera_osd_io *io,uint32_t width,uint32_t height)
{
    if (!s || !io || !io->check_owner || !io->submit || !vmm || isp_fd<0 ||
        width<16+CAMERA_OSD_TIMESTAMP_WIDTH || height<16+CAMERA_OSD_TIMESTAMP_HEIGHT) return -EINVAL;
    if (s->initialized) return -EALREADY;
    *s=(struct camera_osd){.io=*io,.vmm=vmm,.isp_fd=isp_fd,.initialized=1,
        .bitmap={.width=CAMERA_OSD_TIMESTAMP_WIDTH,.height=CAMERA_OSD_TIMESTAMP_HEIGHT,
            .stride=CAMERA_OSD_STRIDE,.x=16,.y=16,.slot=0,.frame_width=width,.frame_height=height}};
    int rc=owned(s);if (rc) goto fail;
    rc=legacy_vmm_allocate(vmm,"clock-osd",CAMERA_OSD_STORAGE,LEGACY_VMM_PAGE,&s->handle);
    if (rc) goto fail;
    s->allocated=1;
    rc=legacy_vmm_map(vmm,s->handle,LEGACY_VMM_MAP_DIRECT);
    if (rc) goto fail;
    s->mapped=1;
    const struct legacy_vmm_block *block=&vmm->blocks[s->handle];
    s->bitmap.physical=block->phys;s->bitmap.bytes=block->size;
    if (!block->mapped || block->user==0 || block->size<CAMERA_OSD_STORAGE) { rc=-EPROTO;goto fail; }
    memset((void *)(uintptr_t)block->user,0,CAMERA_OSD_STORAGE);
    __sync_synchronize();return 0;
fail:
    s->fault=1;return checked(rc);
}
int camera_osd_update_text(struct camera_osd *s,const char text[24])
{
    int rc=owned(s);if (rc) return rc;
    if (s->fault || !s->mapped) return -EPIPE;
    if (!text) return -EINVAL;
    if (s->have_text && !memcmp(s->text,text,24)) return 0;
    uint8_t mask[CAMERA_OSD_TIMESTAMP_WIDTH*CAMERA_OSD_TIMESTAMP_HEIGHT];
    uint8_t packed[CAMERA_OSD_STRIDE*CAMERA_OSD_TIMESTAMP_HEIGHT];
    rc=camera_osd_timestamp_mask(text,mask,sizeof mask,CAMERA_OSD_TIMESTAMP_WIDTH);
    if (rc) return rc;
    rc=camera_osd_timestamp_pack(mask,sizeof mask,CAMERA_OSD_TIMESTAMP_WIDTH,packed,sizeof packed,CAMERA_OSD_STRIDE);
    if (rc) return rc;
    const struct legacy_vmm_block *block=&s->vmm->blocks[s->handle];
    if (!block->mapped || block->phys!=s->bitmap.physical || block->size!=s->bitmap.bytes || !block->user) {
        s->fault=1;return -ESTALE;
    }
    /* Stock glyph writer updates its retained bitmap in place. Local staging
     * minimizes the DMA-visible change interval; it is not a frame-sync fence. */
    __sync_synchronize();
    volatile uint8_t *dst=(volatile uint8_t *)(uintptr_t)block->user;
    for (size_t i=0;i<sizeof packed;i++) dst[i]=packed[i];
    __sync_synchronize();
    memcpy(s->text,text,24);s->have_text=1;return 0;
}
int camera_osd_update_time(struct camera_osd *s,const struct tm *tm)
{
    char text[24];int rc=camera_osd_format_time(tm,text);
    return rc?rc:camera_osd_update_text(s,text);
}
int camera_osd_disable(struct camera_osd *s)
{
    int rc=owned(s);if (rc) return rc;
    if (!s->attempted && !s->enabled) return 0;
    uint32_t wire[CAMERA_GRAPHV2_WORDS];
    rc=camera_graphv2_timestamp(&s->bitmap,0,wire);if (rc) return rc;
    rc=checked(s->io.submit(s->io.opaque,s->isp_fd,CAMERA_GRAPHV2_SET,wire));
    if (rc) { s->fault=1;return rc; }
    s->enabled=s->attempted=0;return 0;
}
int camera_osd_set_enabled(struct camera_osd *s,int enabled)
{
    int rc=owned(s);if (rc) return rc;
    if (enabled!=0 && enabled!=1) return -EINVAL;
    if (!enabled) return camera_osd_disable(s);
    if (s->fault || !s->mapped) return -EPIPE;
    if (!s->have_text) return -ENODATA;
    if (s->enabled) return 0;
    uint32_t wire[CAMERA_GRAPHV2_WORDS];
    rc=camera_graphv2_timestamp(&s->bitmap,1,wire);if (rc) return rc;
    s->attempted=1;
    __sync_synchronize();
    rc=checked(s->io.submit(s->io.opaque,s->isp_fd,CAMERA_GRAPHV2_SET,wire));
    if (rc) { s->fault=1;return rc; }
    s->enabled=1;return 0;
}
