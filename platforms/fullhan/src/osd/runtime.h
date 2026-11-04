#ifndef CAMERAHACK_OSD_RUNTIME_H
#define CAMERAHACK_OSD_RUNTIME_H
#include "timestamp.h"
#include "graphv2.h"
#include "../native/vmm/legacy_vmm.h"
#define CAMERA_OSD_STRIDE 112u
#define CAMERA_OSD_STORAGE 4096u
struct camera_osd_io {
    void *opaque;
    int (*check_owner)(void *);
    int (*submit)(void *,int isp_fd,uint32_t,uint32_t wire[CAMERA_GRAPHV2_WORDS]);
};
struct camera_osd {
    struct camera_osd_io io;
    struct legacy_vmm *vmm;
    struct camera_graphv2_bitmap bitmap;
    size_t handle;
    int isp_fd,initialized,allocated,mapped,enabled,attempted,fault,have_text;
    char text[24];
};
#define CAMERA_OSD_INITIALIZER {0}
/* Fresh zero storage only. Existing exclusive media owner, isp fd and VMM
 * session remain caller-owned. Reserves one4KiB block once; never opens/closes
 * any device, frees a block, resets the zone or restarts media. Starts disabled.
 * Failure retains all tracked resources for whole-pipeline recovery/teardown. */
int camera_osd_init(struct camera_osd *,int isp_fd,struct legacy_vmm *,
                    const struct camera_osd_io *,uint32_t frame_width,uint32_t frame_height);
int camera_osd_update_text(struct camera_osd *,const char text[24]);
int camera_osd_update_time(struct camera_osd *,const struct tm *);
int camera_osd_set_enabled(struct camera_osd *,int enabled);
/* Retry-safe disable packet; never frees/unmaps backing memory. On success an
 * earlier fault remains sticky, so future re-enable requires owner recovery. */
int camera_osd_disable(struct camera_osd *);
#endif
