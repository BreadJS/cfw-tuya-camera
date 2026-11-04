/* MIT; see LICENSE. Pure injected-operation plan, with no device backend. */
#ifndef CAMERHACK_MIS2006_MIPI_H
#define CAMERHACK_MIS2006_MIPI_H
#include <stddef.h>
#include <stdint.h>

enum mis2006_mipi_region { MIPI_SYS, MIPI_MODE, MIPI_PHY, MIPI_MISC, MIPI_REGION_COUNT };
struct mis2006_mipi_mapping { uint32_t physical, length; };
extern const struct mis2006_mipi_mapping mis2006_mipi_mappings[MIPI_REGION_COUNT];
enum mis2006_mipi_step_kind { MIPI_WRITE, MIPI_MASKED_WRITE, MIPI_DELAY_US };
struct mis2006_mipi_step {
    enum mis2006_mipi_step_kind kind;
    enum mis2006_mipi_region region;
    uint32_t offset, keep_mask, value;
};
extern const struct mis2006_mipi_step mis2006_mipi_plan[];
extern const size_t mis2006_mipi_step_count;

/* Return zero only on successful operation, a nonzero error otherwise.
 * map_region must not publish a partial mapping on failure. unmap_region must
 * leave a failed mapping tracked so release can retry. No callbacks are
 * supplied by this component: a hardware backend is a separate integration. */
struct mis2006_mipi_io {
    void *opaque;
    int (*map_region)(void *, enum mis2006_mipi_region, uint32_t, uint32_t);
    int (*unmap_region)(void *, enum mis2006_mipi_region);
    int (*read32)(void *, enum mis2006_mipi_region, uint32_t, uint32_t *);
    int (*write32)(void *, enum mis2006_mipi_region, uint32_t, uint32_t);
    int (*delay_us)(void *, uint32_t);
};
struct mis2006_mipi_state {
    uint32_t mapped_mask;
    size_t completed_steps;
    int configured, poisoned, last_error, cleanup_error;
};
#define MIS2006_MIPI_STATE_INITIALIZER {0}

/* Exact fixed stock MIS2006 path, tuple {0x25,0,1,0xff,UNREAD,2}.
 * The unknown stack word4 is never read by this path. Other tuples are not
 * exposed. Repeated runs retain successful mappings, like stock. */
int mis2006_mipi_run(const struct mis2006_mipi_io *, struct mis2006_mipi_state *);
/* Unmaps only; never claims to roll back hardware register writes. Poison is
 * retained after a failed read/write/delay. A new lifecycle must reestablish
 * hardware reset/ownership before using a fresh zero-initialized state. */
int mis2006_mipi_release(const struct mis2006_mipi_io *, struct mis2006_mipi_state *);
#endif
