/* Concrete source-owned sensor/Linux/MIPI adapter; no implicit owner. */
#ifndef CAMERHACK_MIS2006_LINUX_BRIDGE_H
#define CAMERHACK_MIS2006_LINUX_BRIDGE_H
#include "linux_io.h"
#include "../sensor/mis2006_runtime.h"

struct mis2006_linux_owner {
    void *opaque;
    /* All mandatory, zero/negative. Caller already owns the whole camera.
     * retain adds one component reference; failed retain leaves no reference.
     * release removes only that reference; on error it remains held. check
     * verifies actual exclusion. quiesce establishes all ISP/VPU/encoder DMA
     * stopped and must retain global ownership. Calls are serialized by owner. */
    int (*retain)(void *);
    int (*release)(void *);
    int (*check)(void *);
    int (*quiesce)(void *);
};
struct mis2006_linux_bridge {
    struct fh_linux_io *platform;
    struct mis2006_linux_owner owner;
    struct mis2006_mipi_io mipi_io;
    struct mis2006_mipi_state mipi;
    unsigned retained, prepare_attempted, quiesced;
    unsigned experimental_format204; /* Explicit opt-in to same-clock faster VTS table. */
    int last_error;
};
/* Fresh storage only, or completely cleaned state after verified external reset
 * if poisoned. Constructs callbacks without I/O or acquiring any reference.
 * Platform and owner callbacks must describe the SAME real whole-camera owner.
 * Platform is shared with ISP: this bridge never closes /dev/mem or ISP MMIO.
 * Unknown cleanup must fail; no operation can substitute unmap for quiescence. */
int mis2006_linux_bridge_init(struct mis2006_linux_bridge *,struct mis2006_runtime *,
                              struct fh_linux_io *,const struct mis2006_linux_owner *);
#endif
