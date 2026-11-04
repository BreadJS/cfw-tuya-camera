/* Original adapter for the matching 32-bit legacy VMM ABI. No device backend. */
#ifndef CAMERHACK_LEGACY_VMM_H
#define CAMERHACK_LEGACY_VMM_H
#include <stddef.h>
#include <stdint.h>
#define LEGACY_VMM_ALLOC 0xc04c6d0au
#define LEGACY_VMM_REMAP 0xc04c6d14u
#define LEGACY_VMM_RESET_ZONE 0x404c6d0cu
#define LEGACY_VMM_ZONE_START 0xa2400000u
#define LEGACY_VMM_ZONE_END 0xa3800000u
#define LEGACY_VMM_PAGE 0x1000u
#define LEGACY_VMM_ISP_ALIGNMENT 0x10000u

struct legacy_vmm_request {
    uint32_t phys, alignment, size, reserved_0c, user, flags;
    char name[16];
    char zone[32];
    uint32_t zone_selector;
};
enum legacy_vmm_map_method { LEGACY_VMM_MAP_IOCTL, LEGACY_VMM_MAP_DIRECT };
/* Each successful allocation consumes a caller-provided slot until entire-zone
 * teardown. Do not mutate records or copy a live session. */
struct legacy_vmm_block {
    struct legacy_vmm_request request;
    uint32_t phys, size, user;
    unsigned allocated, mapped;
};
struct legacy_vmm_ops {
    /* Entire VMM zone and all media DMA consumers must be exclusively owned.
     * acquire/release return0 or negative; failed acquire leaves no lease. */
    int (*acquire)(void *);
    int (*release)(void *);
    int (*open)(void *, int *fd); /* fresh private O_RDWR file; never borrowed */
    /* consumed=1: descriptor was closed; 0: certainly still open; -1: unknown.
     * On success consumed MUST be1 and kernel release completed. Never blindly
     * retry a Linux close error: fd consumption must be established by backend. */
    int (*close)(void *, int fd, int *consumed);
    int (*ioctl)(void *, int fd, uint32_t cmd, uint8_t wire[76]);
    /* mmap maps exactly full allocation, uncached, PROT_READ|WRITE, MAP_SHARED.
     * On error it must leave no mapping; on success user is a 32-bit address. */
    int (*mmap)(void *, int fd, uint32_t phys, uint32_t size,
                unsigned prot, unsigned flags, uint32_t *user);
    /* Negative return must leave the complete mapping intact, safe to retry. */
    int (*munmap)(void *, uint32_t user, uint32_t size);
};
struct legacy_vmm {
    struct legacy_vmm_ops ops;
    void *opaque;
    struct legacy_vmm_block *blocks;
    size_t count, capacity;
    int fd;
    unsigned owned, fault, uncertain_allocation, uncertain_mapping, quarantine;
};
/* Fresh/stopped storage only. All calls serialized by owner; no default I/O. */
int legacy_vmm_init(struct legacy_vmm *, const struct legacy_vmm_ops *, void *,
                    struct legacy_vmm_block *, size_t capacity);
int legacy_vmm_open(struct legacy_vmm *);
int legacy_vmm_allocate(struct legacy_vmm *, const char *name, uint32_t size,
                        uint32_t alignment, size_t *handle);
int legacy_vmm_allocate_isp(struct legacy_vmm *, uint32_t size, size_t *handle);
int legacy_vmm_map(struct legacy_vmm *, size_t handle, enum legacy_vmm_map_method);
int legacy_vmm_unmap(struct legacy_vmm *, size_t handle);
/* The driver has NO individual free. This always returns -ENOTSUP. */
int legacy_vmm_free(struct legacy_vmm *, size_t handle);
/* Requires all DMA users quiesced. Unmaps known mappings, then closes the private
 * file, resetting the whole containing zone. Preserves failed cleanup records.
 * Unknown mappings, invalid kernel outputs or an allocation failure with no
 * known block require external recovery; close returns -ENOTRECOVERABLE. */
int legacy_vmm_close(struct legacy_vmm *);
void legacy_vmm_encode(const struct legacy_vmm_request *, uint8_t wire[76]);
void legacy_vmm_decode(const uint8_t wire[76], struct legacy_vmm_request *);
#endif
