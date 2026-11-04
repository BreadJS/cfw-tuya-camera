/* MIT; see LICENSE. Register facts recovered from the matching dgiot image.
 * Semantic reference: OpenIPC/ArthurKoba FH8626 libmipi reimplementation. */
#include "mis2006_mipi.h"
#include <errno.h>

const struct mis2006_mipi_mapping mis2006_mipi_mappings[MIPI_REGION_COUNT] = {
    [MIPI_SYS] = {0xf0000000u, 0x38u},
    [MIPI_MODE] = {0xf1000000u, 0x0cu},
    [MIPI_PHY] = {0xf1100000u, 0x14cu},
    [MIPI_MISC] = {0xf0002000u, 0x44u},
};
static const enum mis2006_mipi_region map_order[] = {
    MIPI_SYS, MIPI_MISC, MIPI_MODE, MIPI_PHY
};
#define W(r,o,v) {MIPI_WRITE, r, o, 0u, v}
#define R(r,o,m,v) {MIPI_MASKED_WRITE, r, o, m, v}
const struct mis2006_mipi_step mis2006_mipi_plan[] = {
    R(MIPI_SYS, 0x0cu, 0xffffffffu, 0x10000u),
    R(MIPI_SYS, 0x24u, 0xffffe3ffu, 0u),
    R(MIPI_SYS, 0x28u, 0x003fff9fu, 0x60u),
    W(MIPI_MISC, 0x40u, 1u),
    W(MIPI_PHY, 0x54u, 0u), W(MIPI_PHY, 0x50u, 1u),
    W(MIPI_PHY, 0x40u, 1u), W(MIPI_PHY, 0x44u, 1u),
    W(MIPI_PHY, 0x08u, 0xffffffffu),
    W(MIPI_PHY, 0x50u, 0u), W(MIPI_PHY, 0x50u, 0u),
    W(MIPI_PHY, 0x54u, 0x10034u), W(MIPI_PHY, 0x50u, 2u), W(MIPI_PHY, 0x50u, 0u),
    W(MIPI_PHY, 0x54u, 0x14u), W(MIPI_PHY, 0x50u, 2u), W(MIPI_PHY, 0x50u, 0u),
    W(MIPI_PHY, 0x54u, 0x10044u), W(MIPI_PHY, 0x50u, 2u), W(MIPI_PHY, 0x50u, 0u),
    W(MIPI_PHY, 0x54u, 0x4au), W(MIPI_PHY, 0x50u, 2u), W(MIPI_PHY, 0x50u, 0u),
    {MIPI_DELAY_US, MIPI_SYS, 0u, 0u, 100u},
    W(MIPI_PHY, 0x04u, 1u), W(MIPI_PHY, 0x04u, 1u),
    W(MIPI_PHY, 0xe4u, 0xffffffffu), W(MIPI_PHY, 0xf4u, 0xffffffffu),
    W(MIPI_PHY, 0x104u, 0xffffffffu), W(MIPI_PHY, 0x114u, 0xffffffffu),
    W(MIPI_PHY, 0x124u, 0xffffffffu), W(MIPI_PHY, 0x134u, 0xffffffffu),
    R(MIPI_MODE, 0u, 0xfffffff8u, 4u), W(MIPI_MODE, 4u, 1u),
};
const size_t mis2006_mipi_step_count = sizeof(mis2006_mipi_plan) / sizeof(mis2006_mipi_plan[0]);
#undef W
#undef R

static int valid_io(const struct mis2006_mipi_io *io)
{
    return io && io->map_region && io->unmap_region && io->read32 &&
        io->write32 && io->delay_us;
}

int mis2006_mipi_release(const struct mis2006_mipi_io *io, struct mis2006_mipi_state *s)
{
    int first = 0;
    if (!valid_io(io) || !s || (s->mapped_mask & ~15u)) return -EINVAL;
    for (size_t i = sizeof(map_order) / sizeof(map_order[0]); i > 0; --i) {
        enum mis2006_mipi_region region = map_order[i - 1];
        if (s->mapped_mask & (1u << region)) {
            int rc = io->unmap_region(io->opaque, region);
            if (!rc) s->mapped_mask &= ~(1u << region);
            else if (!first) first = rc;
        }
    }
    s->configured = 0;
    s->cleanup_error = first;
    return first;
}

int mis2006_mipi_run(const struct mis2006_mipi_io *io, struct mis2006_mipi_state *s)
{
    int rc;
    if (!valid_io(io) || !s || (s->mapped_mask & ~15u)) return -EINVAL;
    if (s->poisoned) return -EPIPE;
    if (s->mapped_mask && s->mapped_mask != 15u) return -EBUSY;
    s->configured = 0;
    s->completed_steps = 0;
    s->last_error = s->cleanup_error = 0;
    if (!s->mapped_mask) {
        for (size_t i = 0; i < sizeof(map_order) / sizeof(map_order[0]); ++i) {
            enum mis2006_mipi_region region = map_order[i];
            const struct mis2006_mipi_mapping *m = &mis2006_mipi_mappings[region];
            rc = io->map_region(io->opaque, region, m->physical, m->length);
            if (rc) goto fail;
            s->mapped_mask |= 1u << region;
        }
    }
    for (size_t i = 0; i < mis2006_mipi_step_count; ++i) {
        const struct mis2006_mipi_step *step = &mis2006_mipi_plan[i];
        uint32_t value = step->value;
        if (step->kind == MIPI_DELAY_US) {
            rc = io->delay_us(io->opaque, step->value);
        } else {
            if (step->kind == MIPI_MASKED_WRITE) {
                rc = io->read32(io->opaque, step->region, step->offset, &value);
                if (rc) goto operation_fail;
                value = (value & step->keep_mask) | step->value;
            }
            rc = io->write32(io->opaque, step->region, step->offset, value);
        }
        if (rc) goto operation_fail;
        s->completed_steps = i + 1;
    }
    s->configured = 1;
    return 0;
operation_fail:
    s->poisoned = 1;
fail:
    s->last_error = rc;
    (void)mis2006_mipi_release(io, s);
    return rc;
}
