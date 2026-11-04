#ifndef CAMERHACK_ISP_MEMORY_H
#define CAMERHACK_ISP_MEMORY_H
#include <stdint.h>

/* This camera's exact 1920x1080 path. These are local records, not SDK structs. */
#define FH_ISP_VMM_START UINT32_C(0xa2400000)
#define FH_ISP_VMM_END UINT32_C(0xa3800000)
#define FH_ISP_ALLOCATION_ALIGNMENT UINT32_C(0x10000)
struct fh_isp_region {
    uint32_t physical, user, bytes;
};
struct fh_isp_memory {
    uint32_t required_bytes, allocation_bytes, used_bytes;
    uint32_t nr3d_mode;
    struct fh_isp_region raw, quarter, statistics;
};

/* mode is the successful driver's query result, not a guessed user setting.
 * Returns the stock requested extent and page-rounded backing extent. */
int fh_isp_memory_size(uint32_t mode, uint32_t *requested, uint32_t *backing);
/* Bind an actual exclusive owner's single VMM allocation. No device access,
 * allocation, mmap or register writes. Output is unchanged on rejection. */
int fh_isp_memory_bind(uint32_t mode, uint32_t physical, uint32_t user,
                       uint32_t backing, struct fh_isp_memory *out);
#define FH_ISP_STAT_BANK_BYTES UINT32_C(0x21270)
#define FH_ISP_ADDRESS_REQUEST_BYTES 92u
/* Exact first-init ioctl0x40016920 input. Revalidates a locally generated plan;
 * no ioctl is issued. A hot-reconfiguration request is outside this API. */
int fh_isp_memory_address_request(const struct fh_isp_memory *,
                                  unsigned char out[FH_ISP_ADDRESS_REQUEST_BYTES]);
/* offset is returned by successful ioctl0x80046905. Reject stale/invalid or
 * partial banks before publishing a CPU/physical span. */
int fh_isp_memory_stat_bank(const struct fh_isp_memory *, uint32_t offset,
                            struct fh_isp_region *out);
#endif
