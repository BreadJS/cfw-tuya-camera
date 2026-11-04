/* Exact matching dgiot path 0x106148 and its three observed helpers. */
#include "isp_geometry.h"
#include <errno.h>

static void operation(struct fh_isp_geometry_plan *p, enum fh_isp_geometry_kind kind,
                      uint32_t offset, uint32_t keep, uint32_t value, unsigned slot)
{
    struct fh_isp_geometry_step *s = &p->steps[p->count++];
    s->kind = kind; s->offset = offset; s->keep_mask = keep; s->value = value; s->slot = slot;
}
#define W(o,v) operation(&plan,FH_ISP_GEOMETRY_WRITE,o,0u,v,0u)
#define R(o,s) operation(&plan,FH_ISP_GEOMETRY_READ,o,0u,0u,s)
#define M(o,k,v,s) operation(&plan,FH_ISP_GEOMETRY_WRITE,o,k,v,s)
int fh_isp_geometry_plan(const struct mis2006_stock_format *format, struct fh_isp_geometry_plan *out)
{
    struct fh_isp_geometry_plan plan = {0};
    uint32_t w, h, sw, sh, x, y, half, sixth_w, sixth_h, block_h, packed;
    uint32_t selector, pixel_format;
    if (!format || !out) return -EINVAL;
    sw = format->sensor_width & 0xfffu; sh = format->sensor_height & 0xfffu;
    w = format->crop_width & 0xfffu; h = format->crop_height & 0xfffu;
    x = format->crop_x & 0xfffu; y = format->crop_y & 0xfffu;
    selector = format->field_14 & 3u;
    pixel_format = (format->field_14 >> 4) & 7u;
    packed = (sw - 1u) | ((sh - 1u) << 16);
    W(0x78u,packed); W(0x30u,packed);
    half = w / 2u; sixth_w = w / 6u; sixth_h = h / 6u;
    block_h = h / 32u - 1u;
    packed = (w - 1u) | ((h - 1u) << 16);
    W(0x80u,packed);
    W(0x490u,((10u * w / 8u) + 7u) & ~7u);
    W(0x488u,((w / 4u) + 7u) & ~7u);
    W(0x1ecu,(block_h << 24) | ((w / 48u - 1u) << 16));
    W(0x5c0u,0u); W(0x5c4u,packed);
    W(0x17cu,(sixth_w - 1u) << 16);
    W(0x180u,(2u * sixth_w - 1u) | ((3u * sixth_w - 1u) << 16));
    W(0x184u,(sixth_h - 1u) << 16);
    W(0x188u,(2u * sixth_h - 1u) | ((3u * sixth_h - 1u) << 16));
    W(0x1a4u,(sixth_w - 1u) << 16);
    W(0x1a8u,(2u * sixth_w - 1u) | ((3u * sixth_w - 1u) << 16));
    W(0x1acu,(sixth_h - 1u) << 16);
    W(0x1b0u,(2u * sixth_h - 1u) | ((3u * sixth_h - 1u) << 16));
    packed = (w / 32u - 1u) | (block_h << 8) | 0x0f0f0000u;
    W(0x1ccu,packed); W(0x1d0u,0u); W(0x1d4u,0x1fff1u);
    W(0x1dcu,packed); W(0x1e0u,0u); W(0x1e4u,0x1fff1u);
    W(0x38cu,(w / 16u) << 16);
    W(0x390u,(w / 8u) | ((3u * half / 8u) << 16));
    W(0x394u,(w / 4u) | ((5u * half / 8u) << 16));
    W(0x398u,(6u * half / 8u) | ((7u * half / 8u) << 16));
    W(0x39cu,half - 1u);
    W(0x3a0u,(h / 8u) | ((h / 4u) << 16));
    W(0x3a4u,(3u * h / 8u) | ((h / 2u - 1u) << 16));
    W(0x3a8u,0x0fff0000u);
    W(0x7cu,x | (y << 16));
    R(0x24u,0u); M(0x24u,0xfffffff9u,selector << 1,0u);
    R(0x24u,0u); M(0x24u,0xffffffe7u,selector << 3,0u);
    R(0x3b4u,0u); R(0x3acu,1u);
    M(0x3b4u,0xffff3ff0u,pixel_format | (selector << 14),0u);
    M(0x3acu,0xfffffff8u,pixel_format,1u);
    *out = plan;
    return 0;
}
#undef W
#undef R
#undef M

int fh_isp_geometry_run(const struct mis2006_stock_format *format,
                        const struct fh_isp_geometry_io *io, struct fh_isp_geometry_state *state)
{
    struct fh_isp_geometry_plan plan;
    uint32_t scratch[2] = {0,0};
    int rc;
    if (!io || !io->read32 || !io->write32 || !state) return -EINVAL;
    if (state->poisoned) return -EPIPE;
    rc = fh_isp_geometry_plan(format,&plan);
    if (rc) return rc;
    state->configured = state->last_error = 0;
    state->completed_operations = 0;
    for (size_t i = 0; i < plan.count; ++i) {
        const struct fh_isp_geometry_step *step = &plan.steps[i];
        if (step->kind == FH_ISP_GEOMETRY_READ)
            rc = io->read32(io->opaque,step->offset,&scratch[step->slot]);
        else {
            uint32_t value = step->value;
            if (step->keep_mask) value |= scratch[step->slot] & step->keep_mask;
            rc = io->write32(io->opaque,step->offset,value);
        }
        if (rc) {
            state->poisoned = 1; state->last_error = rc;
            return rc;
        }
        state->completed_operations = i + 1;
    }
    state->configured = 1;
    return 0;
}
