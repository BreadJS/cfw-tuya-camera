#ifndef CAMERHACK_ISP_FIXED_H
#define CAMERHACK_ISP_FIXED_H
#include <stddef.h>
#include <stdint.h>

/* Local data records, not vendor ABI structs. Each nonzero keep_mask marks
 * a read-modify-write: (old & keep_mask) | value; zero marks a literal write. */
struct fh_isp_fixed_step { uint32_t offset, keep_mask, value; };
extern const struct fh_isp_fixed_step fh_isp_fixed_plan[];
extern const size_t fh_isp_fixed_step_count;
#define FH_ISP_FIXED_REQUIRED_EXTENT UINT32_C(0x4000)
struct fh_isp_fixed_io {
    void *opaque;
    int (*read32)(void *, uint32_t offset, uint32_t *value);
    int (*write32)(void *, uint32_t offset, uint32_t value);
};
struct fh_isp_fixed_state {
    size_t completed_steps;
    int configured, poisoned, last_error;
};
#define FH_ISP_FIXED_STATE_INITIALIZER {0}
/* Caller owns valid exclusive, correctly ordered MMIO access. No mapping or
 * hardware backend is supplied. Callbacks return zero or a negative error.
 * Any failed callback poisons the state; no register rollback is invented.
 * A fresh state requires independently reestablished reset/ownership. */
int fh_isp_fixed_run(const struct fh_isp_fixed_io *, struct fh_isp_fixed_state *);
#endif
