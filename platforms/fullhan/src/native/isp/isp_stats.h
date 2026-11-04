#ifndef CAMERHACK_ISP_STATS_H
#define CAMERHACK_ISP_STATS_H
#include "isp_memory.h"
#include "isp_awb.h"
#include <stddef.h>
#define FH_ISP_STATS_REFRESH UINT32_C(0x40046908)
#define FH_ISP_STATS_OFFSET UINT32_C(0x80046905)
#define FH_ISP_STATS_STATUS UINT32_C(0x8010690e)
struct fh_isp_stats_io {
    void *opaque;
    int (*check)(void *);
    /* Raw result: refresh returns positive remaining-jiffies on success,
     * zero timeout, negative error. Other commands require exactly zero. */
    int (*ioctl)(void *,int,uint32_t,uint8_t *,size_t);
    int (*read32)(void *,uint32_t,uint32_t *);
};
struct fh_isp_stats_sample {
    uint64_t generation,sum,count;
    uint32_t bank_offset,source_irq_counter;
    /* Source mean is SUM/COUNT in the stock0..4096 numeric scale.
     * raw_mean_q16 normalizes that scale; mean_q16 additionally follows
     * stock's isqrt(mean<<12). Neither is decoded/gamma RGB brightness. */
    uint32_t raw_mean_q16,mean_q16;
    unsigned awb_valid;
    struct fh_awb_sample awb;
    /* Same completed generation: original shadow-controller inputs. */
    unsigned shadow_valid;
    uint32_t shadow_width,shadow_height,shadow_histogram[3];
    uint32_t shadow_min_log_q8,shadow_max_log_q8;
};
struct fh_isp_stats {
    struct fh_isp_memory memory;
    struct fh_isp_stats_io io;
    int fd,last_error;
    unsigned initialized,faulted,have_previous;
    uint32_t previous_counter,previous_offset;
    uint64_t generation;
};
/* Existing exclusive owner's mapped memory/privateISPfd only. No open/map,
 * start/stop/close or sensor writes. One producer thread owns this object and
 * must retain the complete ISP/VMM lifetime. Init has no hardware calls. */
int fh_isp_stats_init(struct fh_isp_stats *,const struct fh_isp_memory *,int,
                      const struct fh_isp_stats_io *);
/* May block in vendor refreshRPC up to requested1..100ms timeout (scheduler
 * latency not bounded). Use an owner-controlled thread, not RTSP callback.
 * Zero publishes fresh sample; -EAGAIN publishes nothing, retry later.
 * Hard failures poison this generation; no cleanup/automatic reset supplied. */
int fh_isp_stats_read(struct fh_isp_stats *,unsigned,struct fh_isp_stats_sample *);
int fh_isp_stats_linux_ioctl(void *,int,uint32_t,uint8_t *,size_t);
#endif
