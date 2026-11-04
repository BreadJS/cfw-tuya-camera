/* SPDX-License-Identifier: MIT */
#include "h264_parameters.h"
#include <errno.h>
#include <string.h>

#define SPS_LIMIT 256u
struct bits { uint8_t data[SPS_LIMIT]; size_t bytes, at, critical; };

static int read_bits(struct bits *b, unsigned count, uint32_t *value)
{
    if (count > 32 || count > b->bytes * 8 - b->at) return -EBADMSG;
    *value = 0;
    while (count--) {
        *value = (*value << 1) | ((b->data[b->at / 8] >> (7 - b->at % 8)) & 1);
        b->at++;
    }
    return 0;
}
static int ue(struct bits *b, uint32_t *value)
{
    uint32_t bit, suffix; unsigned zeros = 0;
    do {
        if (read_bits(b, 1, &bit)) return -EBADMSG;
        if (!bit && ++zeros > 30) return -EBADMSG;
    } while (!bit);
    if (read_bits(b, zeros, &suffix)) return -EBADMSG;
    *value = ((1u << zeros) - 1u) + suffix;
    return 0;
}
static int bounded_ue(struct bits *b, uint32_t max, uint32_t *value)
{ return ue(b, value) || *value > max ? -EBADMSG : 0; }
static int se(struct bits *b, int32_t *value)
{
    uint32_t code;
    if (ue(b, &code)) return -EBADMSG;
    *value = (code & 1) ? (int32_t)((code + 1) / 2) : -(int32_t)(code / 2);
    return 0;
}

#define READ(n, value) do { if (read_bits(b, (n), &(value))) return -EBADMSG; } while (0)
#define UE(max, value) do { if (bounded_ue(b, (max), &(value))) return -EBADMSG; } while (0)
#define SE(value) do { if (se(b, &(value))) return -EBADMSG; } while (0)

static int hrd(struct bits *b)
{
    uint32_t count, value;
    UE(31, count);
    READ(4, value); /* bit_rate_scale */
    READ(4, value); /* cpb_size_scale */
    for (uint32_t i = 0; i <= count; i++) {
        UE(UINT32_MAX, value); /* bit_rate_value_minus1 */
        UE(UINT32_MAX, value); /* cpb_size_value_minus1 */
        READ(1, value); /* cbr_flag */
    }
    READ(5, value); READ(5, value); READ(5, value); READ(5, value);
    return 0;
}
static int vui(struct bits *b)
{
    uint32_t flag, value, nal_hrd, vcl_hrd;
    READ(1, flag);
    if (flag) {
        READ(8, value);
        if (value == 255) {
            READ(16, value); if (!value) return -EBADMSG;
            READ(16, value); if (!value) return -EBADMSG;
        } else if (value > 16) return -EBADMSG;
    }
    READ(1, flag); if (flag) READ(1, value); /* overscan */
    READ(1, flag);
    if (flag) {
        READ(3, value); if (value > 5) return -EBADMSG;
        READ(1, value); READ(1, flag);
        if (flag) { READ(8, value); READ(8, value); READ(8, value); }
    }
    READ(1, flag);
    if (flag) { UE(5, value); UE(5, value); }
    READ(1, flag);
    if (flag) {
        READ(32, value); if (!value) return -EBADMSG;
        READ(32, value); if (!value) return -EBADMSG;
        READ(1, value);
    }
    READ(1, nal_hrd); if (nal_hrd && hrd(b)) return -EBADMSG;
    READ(1, vcl_hrd); if (vcl_hrd && hrd(b)) return -EBADMSG;
    if (nal_hrd || vcl_hrd) READ(1, value);
    READ(1, value); /* pic_struct_present_flag */
    READ(1, flag);
    if (flag) {
        uint32_t reorder, buffering;
        READ(1, value);
        UE(16, value); UE(16, value); UE(16, value); UE(16, value);
        UE(16, reorder); UE(16, buffering);
        if (reorder > buffering) return -EBADMSG;
    }
    return 0;
}
static int scaling_list(struct bits *b, unsigned size)
{
    int last = 8, next = 8;
    for (unsigned i = 0; i < size; i++) {
        if (next) {
            int32_t delta;
            SE(delta);
            if (delta < -128 || delta > 127) return -EBADMSG;
            next = (last + delta + 256) % 256;
        }
        if (next) last = next;
    }
    return 0;
}
static int parse_sps(struct bits *b, const uint8_t *data, size_t bytes)
{
    if (!data || bytes < 5) return -EBADMSG;
    if (bytes > SPS_LIMIT) return -EMSGSIZE;
    if ((data[0] & 0x9f) != 7 || !(data[0] & 0x60)) return -EBADMSG;
    memset(b, 0, sizeof *b);
    unsigned zeros = 0;
    for (size_t i = 1; i < bytes; i++) {
        if (zeros == 2) {
            if (data[i] == 3) {
                if (i + 1 == bytes || data[i + 1] > 3) return -EBADMSG;
                zeros = 0; continue;
            }
            if (data[i] < 3) return -EBADMSG;
        }
        b->data[b->bytes++] = data[i];
        zeros = data[i] == 0 ? zeros + 1 : 0;
    }
    uint32_t profile, value, flag, chroma = 1;
    READ(8, profile); READ(8, value);
    if (value & 3) return -EBADMSG; /* reserved_zero_2bits */
    READ(8, value); UE(31, value);
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        UE(3, chroma);
        if (chroma == 3) READ(1, value);
        UE(6, value); UE(6, value); READ(1, value); READ(1, flag);
        if (flag) {
            for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); i++) {
                READ(1, flag);
                if (flag && scaling_list(b, i < 6 ? 16 : 64)) return -EBADMSG;
            }
        }
        break;
    case 66: case 77: case 88: break;
    default: return -EBADMSG;
    }
    UE(12, value); /* log2_max_frame_num_minus4 */
    UE(2, value); /* pic_order_cnt_type */
    if (!value) UE(12, value);
    else if (value == 1) {
        int32_t offset;
        READ(1, flag); SE(offset); SE(offset);
        UE(255, value);
        for (uint32_t i = 0; i < value; i++) SE(offset);
    }
    UE(16, value); READ(1, flag); /* refs, gaps */
    UE(65535, value); UE(65535, value); /* coded dimensions */
    READ(1, flag); if (!flag) READ(1, value);
    READ(1, value); READ(1, flag); /* direct_8x8, cropping */
    if (flag) { UE(65535, value); UE(65535, value); UE(65535, value); UE(65535, value); }
    b->critical = b->at;
    READ(1, flag);
    if (flag && vui(b)) return -EBADMSG;
    READ(1, value); if (value != 1) return -EBADMSG; /* rbsp_stop_one_bit */
    while (b->at % 8) { READ(1, value); if (value) return -EBADMSG; }
    return b->at == b->bytes * 8 ? 0 : -EBADMSG;
}

int camera_h264_sps_compatible(const uint8_t *old, size_t old_bytes,
                               const uint8_t *next, size_t next_bytes)
{
    struct bits a, b;
    int rc = parse_sps(&a, old, old_bytes);
    if (!rc) rc = parse_sps(&b, next, next_bytes);
    if (rc) return rc;
    if (old[0] != next[0] || a.critical != b.critical) return -EPROTO;
    size_t whole = a.critical / 8;
    unsigned remainder = a.critical % 8;
    if (memcmp(a.data, b.data, whole)) return -EPROTO;
    if (remainder && ((a.data[whole] ^ b.data[whole]) & (0xffu << (8 - remainder)))) return -EPROTO;
    return 0;
}
