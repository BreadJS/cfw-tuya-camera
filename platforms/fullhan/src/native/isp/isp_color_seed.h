#ifndef CAMERHACK_ISP_COLOR_SEED_H
#define CAMERHACK_ISP_COLOR_SEED_H
#include "../platform/linux_io.h"

#define FH_ISP_COLOR_SEED_V1_BYTES 32U
#define FH_ISP_COLOR_SEED_BYTES 92U
#define FH_ISP_COLOR_SEED_MAGIC UINT32_C(0x4c4f4346) /* Wire ASCII FCOL. */
struct fh_isp_color_seed {
    uint32_t magic,version,width,height,reg24,gain01,gain23,clip;
    uint32_t blc[2],ccm[6],yc[7];
};
struct fh_isp_color_seed_state {
    unsigned attempted,applied,faulted,writes_completed;
    uint32_t previous24,applied24;
    int last_error;
};
/* Exact32-byte v1 or92-byte v2 little-endian input. Decode output unchanged on rejection. */
int fh_isp_color_seed_decode(const uint8_t *,size_t,struct fh_isp_color_seed *);
/* Explicit source file only: regular,root-owned,no group/other writes,exact versioned size,
 * final-component symlink rejected. No default path or hardware access. */
int fh_isp_color_seed_read(const char *,struct fh_isp_color_seed *);
/* Optional board/scene-specific static seed, NOT an AE/AWB controller/profile.
 * Caller must independently establish producer disabled and correct lifecycle
 * before calling, and retain exclusive owned Linux MMIO throughout. A fresh
 * state is required. Checks format/Bayer bits0x1e against current reg24, keeps
 * current bit23. V1 writes ONLY224/228/4bc/24; V2 also writes BLC84/88,
 * complete CCM4c0..4d4 and YC4dc..4e8,5c8/cc/d0 (flags24 last). No IRQ/DMA-address
 * write, producer enable, rollback, unmap or close is provided. A partial
 * failure retains explicit progress; it is not proof of restored image state.
 * Other reg24 flag meanings are not asserted by copying the observed word. */
int fh_isp_color_seed_apply(const struct fh_isp_color_seed *,struct fh_linux_io *,
                            struct fh_isp_color_seed_state *);
#endif
