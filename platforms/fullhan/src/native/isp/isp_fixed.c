/* Fixed trace recovered from matching stock dgiot; see ISP_FIXED_EVIDENCE.md. */
#include "isp_fixed.h"
#include <errno.h>

const struct fh_isp_fixed_step fh_isp_fixed_plan[] = {
#include "isp_fixed_plan.inc"
};
const size_t fh_isp_fixed_step_count = sizeof(fh_isp_fixed_plan) / sizeof(fh_isp_fixed_plan[0]);

int fh_isp_fixed_run(const struct fh_isp_fixed_io *io, struct fh_isp_fixed_state *state)
{
    int rc;
    if (!io || !io->read32 || !io->write32 || !state) return -EINVAL;
    if (state->poisoned) return -EPIPE;
    state->completed_steps = 0;
    state->configured = state->last_error = 0;
    for (size_t i = 0; i < fh_isp_fixed_step_count; ++i) {
        const struct fh_isp_fixed_step *step = &fh_isp_fixed_plan[i];
        uint32_t value = step->value;
        if (step->keep_mask) {
            rc = io->read32(io->opaque, step->offset, &value);
            if (rc) goto fail;
            value = (value & step->keep_mask) | step->value;
        }
        rc = io->write32(io->opaque, step->offset, value);
        if (rc) goto fail;
        state->completed_steps = i + 1;
    }
    state->configured = 1;
    return 0;
fail:
    state->poisoned = 1;
    state->last_error = rc;
    return rc;
}
