/* Linux backend, invoked only by an explicitly constructed native owner. */
#ifndef CAMERHACK_LEGACY_VMM_POSIX_H
#define CAMERHACK_LEGACY_VMM_POSIX_H
#include "legacy_vmm.h"

struct legacy_vmm_zone_lease {
    void *opaque;
    /* Mandatory verified whole-zone/media ownership implementation. Zero on
     * success, negative on failure. No fd borrowing, external VMAs or active
     * DMA consumers; failed acquire leaves no lease. No default lock supplied. */
    int (*acquire)(void *);
    int (*release)(void *);
};
struct legacy_vmm_posix {
    struct legacy_vmm_zone_lease lease;
    int fd;
    int last_syscall_error; /* negative errno from latest failed syscall */
    unsigned lease_held;
};
/* Fresh storage only; performs no syscall and does not acquire ownership. */
int legacy_vmm_posix_init(struct legacy_vmm_posix *, const struct legacy_vmm_zone_lease *);
/* Pass these ops with this backend as opaque to legacy_vmm_init. Do not invoke
 * callbacks directly or mutate backend fields. Session owns all resource records.
 * Linux only; caller serializes the entire lifetime and forbids fork/dup/foreign
 * close/mapping changes. No executable or constructor opens a device. */
const struct legacy_vmm_ops *legacy_vmm_posix_ops(void);
#endif
