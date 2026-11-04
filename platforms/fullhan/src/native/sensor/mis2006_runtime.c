#include "mis2006_runtime.h"
#include <errno.h>
#include <string.h>

static int checked(int rc) { return rc > 0 ? -EIO : rc; }
static int fault(struct mis2006_runtime *s, int rc)
{
    rc = checked(rc);
    if (rc) s->state = MIS2006_FAULT;
    return rc;
}
static int active(struct mis2006_runtime *s)
{ return !s ? -EINVAL : s->state == MIS2006_ACTIVE ? 0 : -EPIPE; }
static int write_reg(struct mis2006_runtime *s, struct mis2006_reg r)
{
    uint8_t wire[3];
    mis2006_encode_write(&r, wire);
    return fault(s, s->io.write(s->opaque, wire));
}
static int read_reg(struct mis2006_runtime *s, uint16_t reg, uint8_t *value)
{
    uint8_t wire[2], v;
    int rc;
    mis2006_encode_read_address(reg, wire);
    rc = checked(s->io.read(s->opaque, wire, &v));
    if (!rc) *value = v;
    return rc;
}
int mis2006_runtime_init(struct mis2006_runtime *s, const struct mis2006_io *io, void *opaque)
{
    if (!s || !io) return -EINVAL;
    if (!io->acquire || !io->release || !io->prepare || !io->unprepare ||
        !io->bus_open || !io->bus_close || !io->write || !io->read ||
        !io->delay_us || !io->mipi_configure || !io->mipi_disable) return -ENOTSUP;
    memset(s, 0, sizeof(*s));
    s->io = *io;
    s->opaque = opaque;
    return 0;
}
int mis2006_stop(struct mis2006_runtime *s)
{
    int rc;
    if (!s || !s->io.release) return -EINVAL;
    s->state = MIS2006_FAULT;
    s->last_cleanup_error = 0;
    /* Stop at the first failure, retaining all lower-level resources. */
#define CLEAN(flag, callback) do { if (s->flag) { \
    rc = checked(s->io.callback(s->opaque)); \
    if (rc) { s->last_cleanup_error = rc; return rc; } s->flag = 0; } } while (0)
    CLEAN(mipi, mipi_disable);
    CLEAN(bus, bus_close);
    CLEAN(prepared, unprepare);
    CLEAN(owned, release);
#undef CLEAN
    s->state = MIS2006_IDLE;
    s->addr7 = 0;
    s->format = s->gain_q6 = s->exposure = 0;
    return 0;
}
int mis2006_set_exposure(struct mis2006_runtime *s, uint32_t lines)
{
    struct mis2006_stock_exposure p;
    int rc = active(s);
    if (rc) return rc;
    /* New runtime safety policy: reject logical/wire truncation. Frame-relative
     * integration bounds must still be established by the platform controller. */
    if (lines > 0xffffu) return -ERANGE;
    mis2006_stock_exposure_plan(lines, &p);
    if ((rc = write_reg(s, p.writes[0])) || (rc = write_reg(s, p.writes[1]))) return rc;
    s->exposure = p.effective_lines;
    return 0;
}
int mis2006_set_gain(struct mis2006_runtime *s, uint32_t q6)
{
    struct mis2006_stock_gain p;
    int rc = active(s);
    if (rc) return rc;
    mis2006_stock_gain_plan(q6, &p);
    if ((rc = write_reg(s, p.gain_3102)) || (rc = write_reg(s, p.correction_3945))) return rc;
    s->gain_q6 = p.effective_q6;
    return 0;
}
int mis2006_set_mode(struct mis2006_runtime *s, uint32_t format)
{
    struct mis2006_stock_format f;
    struct mis2006_mode_plan p;
    struct mis2006_mode_step step;
    size_t i;
    int rc = active(s);
    if (rc) return rc;
    memset(&f, 0, sizeof(f)); /* Reserved bytes are new API, not vendor ABI. */
    if (mis2006_stock_get_format(format, &f) || mis2006_stock_mode_plan(format, 1, &p)) return -EINVAL;
    s->mipi = 1; /* cleanup required even if configure fails partially */
    rc = fault(s, s->io.mipi_configure(s->opaque, format, &f));
    if (rc) return rc;
    for (i = 0; i < p.step_count; ++i) {
        if (mis2006_stock_mode_step(&p, i, &step)) return fault(s, -EINVAL);
        switch (step.kind) {
        case MIS2006_MODE_WRITE_REG: rc = write_reg(s, step.reg); break;
        case MIS2006_MODE_DELAY_US: rc = fault(s, s->io.delay_us(s->opaque, step.argument)); break;
        case MIS2006_MODE_SET_EXPOSURE: rc = mis2006_set_exposure(s, step.argument); break;
        case MIS2006_MODE_SET_GAIN_Q6: rc = mis2006_set_gain(s, step.argument); break;
        default: rc = fault(s, -ENOTSUP); break;
        }
        if (rc) return rc;
    }
    s->format = format;
    return 0;
}
int mis2006_start(struct mis2006_runtime *s, uint32_t format)
{
    enum mis2006_stock_mode mode;
    uint8_t high, low;
    unsigned i;
    int rc;
    if (!s || !s->io.acquire) return -EINVAL;
    if (s->state != MIS2006_IDLE || s->owned) return -EBUSY;
    if (mis2006_stock_format_mode(format, &mode)) return -EINVAL;
    s->last_cleanup_error = 0;
    rc = checked(s->io.acquire(s->opaque));
    if (rc) return rc;
    s->owned = s->prepared = 1;
    rc = checked(s->io.prepare(s->opaque));
    if (rc) goto fail;
    for (i = 0; i < 2; ++i) {
        s->bus = 1;
        s->addr7 = mis2006_stock_detect_addr7[i];
        rc = checked(s->io.bus_open(s->opaque, s->addr7));
        if (rc) goto fail;
        rc = read_reg(s, MIS2006_CHIP_ID_H_REG, &high);
        if (!rc) rc = read_reg(s, MIS2006_CHIP_ID_L_REG, &low);
        /* Transport failure aborts; only clean ID mismatch tries next address. */
        if (rc) goto fail;
        if (mis2006_chip_id_matches(high, low)) break;
        rc = checked(s->io.bus_close(s->opaque));
        if (rc) goto fail;
        s->bus = 0;
    }
    if (i == 2) { rc = -ENODEV; goto fail; }
    s->state = MIS2006_ACTIVE;
    rc = mis2006_set_mode(s, format);
    if (!rc) return 0;
fail:
    s->state = MIS2006_FAULT;
    (void)mis2006_stop(s); /* primary failure returned; cleanup error retained */
    return rc;
}
int mis2006_set_orientation(struct mis2006_runtime *s, uint32_t bits)
{
    struct mis2006_stock_orientation p;
    struct mis2006_reg writes[5];
    unsigned i;
    int rc = active(s);
    if (rc) return rc;
    if (bits > 3) return -EINVAL;
    mis2006_stock_orientation_plan(bits, &p);
    writes[0] = p.mode_3007; writes[1] = p.window_3205; writes[2] = p.window_3207;
    writes[3] = p.window_3209; writes[4] = p.window_320b;
    for (i = 0; i < 5; ++i) if ((rc = write_reg(s, writes[i]))) return rc;
    return 0;
}
int mis2006_set_rotation(struct mis2006_runtime *s,unsigned rotate_180)
{
    if(rotate_180>1)return -EINVAL;
    struct mis2006_stock_orientation plan;
    unsigned bits=rotate_180?0:3;
    mis2006_stock_orientation_plan(bits,&plan);
    int rc=mis2006_set_orientation(s,bits);if(rc)return rc;
    const struct mis2006_reg expected[5]={plan.mode_3007,plan.window_3205,plan.window_3207,plan.window_3209,plan.window_320b};
    for(unsigned i=0;i<5;i++){
        uint8_t value;rc=mis2006_read_register(s,expected[i].address,&value);if(rc)return rc;
        if(value!=expected[i].value)return fault(s,-EPROTO);
    }
    return 0;
}
int mis2006_read_register(struct mis2006_runtime *s, uint16_t reg, uint8_t *value)
{
    int rc = active(s);
    if (rc) return rc;
    if (!value) return -EINVAL;
    return fault(s, read_reg(s, reg, value));
}
int mis2006_write_register(struct mis2006_runtime *s, uint16_t reg, uint8_t value)
{
    struct mis2006_reg r;
    int rc = active(s);
    if (rc) return rc;
    r.address = reg; r.value = value;
    rc = write_reg(s, r);
    s->state = MIS2006_FAULT;
    return rc;
}
int mis2006_get_gain(struct mis2006_runtime *s, uint32_t *q6)
{
    uint8_t v;
    struct mis2006_reg r;
    int rc = active(s);
    if (rc) return rc;
    if (!q6) return -EINVAL;
    if ((rc = mis2006_read_register(s, 0x3802, &v))) return rc;
    mis2006_stock_gain_query_correction(v, &r);
    if ((rc = write_reg(s, r))) return rc;
    *q6 = s->gain_q6;
    return 0;
}
int mis2006_get_orientation(struct mis2006_runtime *s, unsigned *bit1)
{
    uint8_t v;
    int rc = active(s);
    if (rc) return rc;
    if (!bit1) return -EINVAL;
    if ((rc = mis2006_read_register(s, 0x3007, &v))) return rc;
    *bit1 = (unsigned)mis2006_stock_orientation_query(v);
    return 0;
}
