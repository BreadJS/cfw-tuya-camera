#ifndef CAMERHACK_ISP_STARTUP_H
#define CAMERHACK_ISP_STARTUP_H
#include "isp_memory.h"
#include "isp_fixed.h"
#include "isp_geometry.h"
#include "../platform/linux_io.h"
#include "../capture/fh_capture.h"

#define FH_ISP_QUERY_NR3D 0x80206926u
#define FH_ISP_BUFFER_MODE 0x40016921u
#define FH_ISP_ADDRESS 0x40016920u
#define FH_ISP_VPU_PACKAGE 0x4004692fu
#define FH_ISP_RESET_COUNTERS 0x00006919u
#define FH_ISP_ONCE_INIT 0x00006932u
struct fh_isp_control_io {
    void *opaque;
    int (*ioctl)(void *, int, uint32_t, uint8_t *, size_t);
};
enum fh_isp_stage {
    FH_ISP_NEW, FH_ISP_QUERY, FH_ISP_ALLOCATE, FH_ISP_MAP,
    FH_ISP_BIND, FH_ISP_MODE, FH_ISP_ADDRESSES, FH_ISP_PACKAGE,
    FH_ISP_COUNTERS, FH_ISP_ONCE, FH_ISP_MMIO, FH_ISP_FIXED,
    FH_ISP_MASKED, FH_ISP_GEOMETRY, FH_ISP_PREPARED, FH_ISP_ENABLE
};
struct fh_isp_startup {
    struct fh_capture *capture;
    struct fh_linux_io *platform;
    struct fh_isp_control_io io;
    struct fh_isp_memory memory;
    struct fh_isp_fixed_state fixed;
    struct fh_isp_geometry_state geometry;
    uint32_t capabilities[8], package_user;
    size_t allocation;
    unsigned retained, prepared, producer_attempted, producer_ack, fault;
    enum fh_isp_stage last_completed, uncertain_stage;
    int last_error;
};
/* Fresh storage. Caller already holds whole-zone/media ownership, supplies
 * actual private fds, prepared VPU/PAE systems and VPU channel memory, then
 * configured native VI and obtained the VPU-enable ACK before ISP setup.
 * No hardware call during init; there is no recovery/release implementation. */
int fh_isp_startup_init(struct fh_isp_startup *, struct fh_capture *,
                        struct fh_linux_io *, const struct fh_isp_control_io *);
/* Exact live buffer-mode0 path, queried capability0/1, owned legacy VMM buffer,
 * fixed registers and sensor geometry. Keeps producer interrupt mask cleared.
 * Profile/AE/AWB/control-loop integration is NOT supplied by this component. */
int fh_isp_startup_prepare(struct fh_isp_startup *, const struct mis2006_stock_format *);
/* Actual owner sequencing: memory first, then externally initialize the sensor
 * and MIPI, then fixed registers/geometry. The combined helper above has no
 * sensor callback and is not a complete camera initializer. The caller must
 * verify sensor setup succeeded before calling prepare_registers. */
int fh_isp_startup_prepare_memory(struct fh_isp_startup *);
int fh_isp_startup_prepare_registers(struct fh_isp_startup *,
                                    const struct mis2006_stock_format *);
/* Called only after configured encoder routing exists. Actual first-frame
 * production must be checked by the owner; ACK here is only completed writes. */
int fh_isp_startup_enable(struct fh_isp_startup *);
int fh_isp_control_linux_ioctl(void *, int, uint32_t, uint8_t *, size_t);
#endif
