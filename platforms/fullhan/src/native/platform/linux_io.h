#ifndef CAMERHACK_LINUX_IO_H
#define CAMERHACK_LINUX_IO_H
#include "../mipi/mis2006_mipi.h"
#include "../isp/isp_fixed.h"
#include "../isp/isp_geometry.h"
#include <stddef.h>
#include <stdint.h>

struct fh_linux_mapping { void *address; size_t extent; unsigned mapped; };
struct fh_linux_io {
    void *owner;
    /* check must establish a currently held exclusive whole-camera lease.
     * quiesce must stop all ISP/VPU/encoder DMA users before cleanup. Neither
     * callback is supplied here; a positive return is an error. */
    int (*check)(void *);
    int (*quiesce)(void *);
    int mem_fd, i2c_fd;
    uint8_t i2c_address;
    unsigned touched, quarantined;
    struct fh_linux_mapping mipi[MIPI_REGION_COUNT], isp;
};
/* Fresh or fully closed storage only: re-init must not discard live resources.
 * No syscall/hardware effects during init or callback construction. */
int fh_linux_io_init(struct fh_linux_io *, void *, int (*)(void *), int (*)(void *));
int fh_linux_io_mipi_ops(struct fh_linux_io *, struct mis2006_mipi_io *);
int fh_linux_io_fixed_ops(struct fh_linux_io *, struct fh_isp_fixed_io *);
int fh_linux_io_geometry_ops(struct fh_linux_io *, struct fh_isp_geometry_io *);
int fh_linux_io_map_isp(struct fh_linux_io *);
int fh_linux_io_isp_read(void *, uint32_t, uint32_t *);
int fh_linux_io_isp_write(void *, uint32_t, uint32_t);
int fh_linux_io_bus_open(void *, uint8_t);
int fh_linux_io_bus_close(void *);
int fh_linux_io_bus_write(void *, const uint8_t [3]);
int fh_linux_io_bus_read(void *, const uint8_t [2], uint8_t *);
int fh_linux_io_delay(void *, uint32_t);
/* Calls required quiescence before releasing touched hardware mappings.
 * Failed cleanup retains mappings and reports failure; no lease is released. */
int fh_linux_io_close(struct fh_linux_io *);
#endif
