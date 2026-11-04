#ifndef CAMERHACK_ISP_GEOMETRY_H
#define CAMERHACK_ISP_GEOMETRY_H
#include "../sensor/mis2006_contract.h"
#include <stddef.h>
#include <stdint.h>

/* Source-owned operations, not the vendor context or callback ABI. READ stores
 * a register in scratch[slot]; WRITE uses (scratch[slot]&keep_mask)|value when
 * keep_mask != 0, otherwise it writes the literal value. */
enum fh_isp_geometry_kind { FH_ISP_GEOMETRY_READ, FH_ISP_GEOMETRY_WRITE };
struct fh_isp_geometry_step {
    enum fh_isp_geometry_kind kind;
    uint32_t offset, keep_mask, value;
    unsigned slot;
};
#define FH_ISP_GEOMETRY_MAX_STEPS 48u
#define FH_ISP_GEOMETRY_REQUIRED_EXTENT UINT32_C(0x5c8)
struct fh_isp_geometry_plan {
    size_t count;
    struct fh_isp_geometry_step steps[FH_ISP_GEOMETRY_MAX_STEPS];
};
int fh_isp_geometry_plan(const struct mis2006_stock_format *, struct fh_isp_geometry_plan *);
struct fh_isp_geometry_io {
    void *opaque;
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
};
struct fh_isp_geometry_state {
    size_t completed_operations;
    int configured, poisoned, last_error;
};
#define FH_ISP_GEOMETRY_STATE_INITIALIZER {0}
/* Exclusive ownership, valid mapping and ordered accesses are caller contracts.
 * Any callback failure poisons state; a fresh state needs independent hardware
 * reset/recovery. No mapping, backend, rollback or shutdown is provided. */
int fh_isp_geometry_run(const struct mis2006_stock_format *, const struct fh_isp_geometry_io *, struct fh_isp_geometry_state *);
#endif
