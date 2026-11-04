#include "mis2006_linux_bridge.h"
#include <errno.h>
#include <string.h>

static int checked(int rc) { return rc>0 ? -EIO : rc; }
static int remember(struct mis2006_linux_bridge *b,int rc)
{ rc=checked(rc);if (rc) b->last_error=rc;return rc; }
static int check(struct mis2006_linux_bridge *b)
{
    int rc;
    if (!b || !b->platform || !b->retained) return -EPERM;
    if (b->platform->quarantined) return -ENOTRECOVERABLE;
    rc=checked(b->owner.check(b->owner.opaque));
    if (!rc) rc=checked(b->platform->check(b->platform->owner));
    return remember(b,rc);
}
static int acquire(void *opaque)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc;
    if (!b || !b->platform) return -EINVAL;
    if (b->retained || b->prepare_attempted || b->mipi.mapped_mask) return -EBUSY;
    if (b->mipi.poisoned || b->platform->quarantined) return -EPIPE;
    rc=checked(b->owner.check(b->owner.opaque));
    if (!rc) rc=checked(b->platform->check(b->platform->owner));
    if (rc) return remember(b,rc);
    rc=checked(b->owner.retain(b->owner.opaque));
    if (rc) return remember(b,rc);
    b->retained=1;b->quiesced=0;
    return 0;
}
static int quiesce(struct mis2006_linux_bridge *b)
{
    int rc=check(b);
    if (rc) return rc;
    if (!b->quiesced) {
        rc=remember(b,b->owner.quiesce(b->owner.opaque));
        if (rc) return rc;
        b->quiesced=1;
    }
    return 0;
}
static int release(void *opaque)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    if (rc) return rc;
    if (!b->quiesced || b->prepare_attempted || b->mipi.mapped_mask ||
        b->mipi.configured || b->platform->i2c_fd>=0) return remember(b,-EBUSY);
    for (unsigned i=0;i<MIPI_REGION_COUNT;++i)
        if (b->platform->mipi[i].mapped) return remember(b,-EBUSY);
    rc=remember(b,b->owner.release(b->owner.opaque));
    if (!rc) b->retained=0;
    return rc;
}
static int prepare(void *opaque)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    if (rc) return rc;
    if (b->prepare_attempted) return remember(b,-EALREADY);
    b->prepare_attempted=1;b->quiesced=0;
    /* Exact stock fixed preparation belongs BEFORE bus/ID detection. */
    return remember(b,mis2006_mipi_run(&b->mipi_io,&b->mipi));
}
static int disable(void *opaque)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=quiesce(b);
    if (rc) return rc;
    /* CPU unmapping follows independently proven hardware quiescence. */
    return remember(b,mis2006_mipi_release(&b->mipi_io,&b->mipi));
}
static int unprepare(void *opaque)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=disable(b);
    /* Required even on failed prepare before runtime's later mipi flag. */
    if (!rc) b->prepare_attempted=0;
    return rc;
}
static int bus_open(void *opaque,uint8_t address)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    if (rc) return rc;
    if (!b->prepare_attempted || !b->mipi.configured || b->mipi.poisoned) return -EPIPE;
    b->quiesced=0;
    return remember(b,fh_linux_io_bus_open(b->platform,address));
}
static int bus_close(void *opaque)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    if (rc) return rc;
    return remember(b,fh_linux_io_bus_close(b->platform));
}
static int write_sensor(void *opaque,const uint8_t wire[3])
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    if (rc) return rc;
    b->quiesced=0;
    return remember(b,fh_linux_io_bus_write(b->platform,wire));
}
static int read_sensor(void *opaque,const uint8_t address[2],uint8_t *value)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    if (rc) return rc;
    b->quiesced=0; /* I2C address phase itself can change device state. */
    return remember(b,fh_linux_io_bus_read(b->platform,address,value));
}
static int delay(void *opaque,uint32_t usec)
{
    struct mis2006_linux_bridge *b=opaque;
    int rc=check(b);
    return rc ? rc : remember(b,fh_linux_io_delay(b->platform,usec));
}
static int configure(void *opaque,uint32_t code,const struct mis2006_stock_format *format)
{
    struct mis2006_linux_bridge *b=opaque;
    struct mis2006_stock_format expected={0};
    int rc=check(b);
    if (rc) return rc;
    if (code!=MIS2006_FORMAT_201 && !(code==MIS2006_FORMAT_204 && b->experimental_format204))
        return remember(b,-ENOTSUP);
    if (!format || !b->prepare_attempted || !b->mipi.configured || b->mipi.poisoned) return -EPIPE;
    mis2006_stock_get_format(code,&expected);
    if (format->vts!=expected.vts || format->hts!=expected.hts ||
        format->sensor_width!=expected.sensor_width || format->sensor_height!=expected.sensor_height ||
        format->crop_x!=expected.crop_x || format->crop_y!=expected.crop_y ||
        format->crop_width!=expected.crop_width || format->crop_height!=expected.crop_height ||
        format->field_14!=expected.field_14) return remember(b,-EINVAL);
    /* Only validates this exact fixed-path equivalence; NEVER reruns MIPI. */
    return 0;
}
int mis2006_linux_bridge_init(struct mis2006_linux_bridge *b,struct mis2006_runtime *sensor,
                              struct fh_linux_io *platform,const struct mis2006_linux_owner *owner)
{
    static const struct mis2006_io io={acquire,release,prepare,unprepare,bus_open,bus_close,
        write_sensor,read_sensor,delay,configure,disable};
    int rc;
    if (!b || !sensor || !platform || !owner) return -EINVAL;
    if (!owner->retain || !owner->release || !owner->check || !owner->quiesce ||
        !platform->check || !platform->quiesce) return -ENOTSUP;
    if (platform->i2c_fd>=0 || platform->quarantined) return -EBUSY;
    for (unsigned i=0;i<MIPI_REGION_COUNT;++i) if (platform->mipi[i].mapped) return -EBUSY;
    memset(b,0,sizeof(*b));b->platform=platform;b->owner=*owner;
    rc=fh_linux_io_mipi_ops(platform,&b->mipi_io);
    if (rc) return remember(b,rc);
    return mis2006_runtime_init(sensor,&io,b);
}
