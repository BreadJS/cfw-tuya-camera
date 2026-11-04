#include "isp_memory.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

#define WIDTH UINT32_C(1920)
#define HEIGHT UINT32_C(1080)
#define STATS_BYTES UINT32_C(0x42500)
#define RESERVED_BYTES UINT32_C(0xa00)
static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
static uint32_t raw_size(void)
{
    return HEIGHT * align_up((WIDTH * 10) >> 3, 8);
}
static uint32_t quarter_size(void)
{
    return (HEIGHT >> 2) * align_up(WIDTH >> 2, 8);
}
int fh_isp_memory_size(uint32_t mode, uint32_t *requested, uint32_t *backing)
{
    if (!requested || !backing || requested == backing) return -EINVAL;
    uint32_t need = STATS_BYTES + RESERVED_BYTES;
    if (mode) need += align_up(raw_size(), 128) + align_up(quarter_size(), 128);
    *requested = need;
    *backing = align_up(need, 4096);
    return 0;
}
static void region(struct fh_isp_region *out, uint32_t physical, uint32_t user,
                   uint32_t offset, uint32_t bytes)
{
    *out = (struct fh_isp_region){physical + offset, user + offset, bytes};
}
int fh_isp_memory_bind(uint32_t mode, uint32_t physical, uint32_t user,
                       uint32_t backing, struct fh_isp_memory *out)
{
    uint32_t requested, minimum;
    if (!out || !user || (user & 4095) ||
        (physical & (FH_ISP_ALLOCATION_ALIGNMENT - 1)) || (backing & 4095))
        return -EINVAL;
    if (physical < FH_ISP_VMM_START || physical >= FH_ISP_VMM_END ||
        backing > FH_ISP_VMM_END - physical ||
        (uint64_t)user + backing > (UINT64_C(1) << 32)) return -ERANGE;
    int rc = fh_isp_memory_size(mode, &requested, &minimum);
    if (rc) return rc;
    if (backing < minimum) return -ENOMEM;
    struct fh_isp_memory result = {0};
    result.required_bytes = requested;
    result.allocation_bytes = backing;
    result.nr3d_mode = mode;
    uint32_t cursor = 0;
    if (mode) {
        region(&result.raw, physical, user, cursor, raw_size());
        cursor = align_up(cursor + result.raw.bytes, 128);
        region(&result.quarter, physical, user, cursor, quarter_size());
        cursor = align_up(cursor + result.quarter.bytes, 128);
    }
    region(&result.statistics, physical, user, cursor, STATS_BYTES);
    result.used_bytes = cursor + STATS_BYTES;
    if (result.used_bytes > requested || requested - result.used_bytes != RESERVED_BYTES)
        return -ERANGE;
    *out = result;
    return 0;
}
static int validate(const struct fh_isp_memory *m)
{
    if (!m) return -EINVAL;
    uint32_t physical = m->nr3d_mode ? m->raw.physical : m->statistics.physical;
    uint32_t user = m->nr3d_mode ? m->raw.user : m->statistics.user;
    struct fh_isp_memory expected;
    int rc = fh_isp_memory_bind(m->nr3d_mode, physical, user,
                                 m->allocation_bytes, &expected);
    if (rc) return rc;
    return memcmp(m, &expected, sizeof expected) ? -EINVAL : 0;
}
static void put_le32(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) out[i] = (unsigned char)(value >> (8 * i));
}
int fh_isp_memory_address_request(const struct fh_isp_memory *m,
                                  unsigned char out[FH_ISP_ADDRESS_REQUEST_BYTES])
{
    if (!out) return -EINVAL;
    int rc = validate(m);
    if (rc) return rc;
    unsigned char request[FH_ISP_ADDRESS_REQUEST_BYTES] = {0};
    put_le32(request + 0x08, m->statistics.physical);
    put_le32(request + 0x28, m->raw.physical);
    put_le32(request + 0x48, m->quarter.physical);
    memcpy(out, request, sizeof request);
    return 0;
}
int fh_isp_memory_stat_bank(const struct fh_isp_memory *m, uint32_t offset,
                            struct fh_isp_region *out)
{
    if (!out) return -EINVAL;
    int rc = validate(m);
    if (rc) return rc;
    if (offset % FH_ISP_STAT_BANK_BYTES || offset > m->statistics.bytes ||
        FH_ISP_STAT_BANK_BYTES > m->statistics.bytes - offset) return -ERANGE;
    struct fh_isp_region bank;
    region(&bank, m->statistics.physical, m->statistics.user, offset,
           FH_ISP_STAT_BANK_BYTES);
    *out = bank;
    return 0;
}
