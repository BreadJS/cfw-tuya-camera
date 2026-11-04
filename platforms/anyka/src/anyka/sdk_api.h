/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_SDK_API_H
#define CAMERA_ANYKA_SDK_API_H

#include "sdk_abi.h"

typedef void *(*anyka_symbol_lookup)(void *opaque, const char *name);

/* Binding checks exact pure version getters and resolves symbols. It does not
 * initialize memory, open drivers, change ISP state or prove runtime ABI safety.
 * An exclusively owned, separately reviewed trial must prove the hardware path.
 */
struct anyka_sdk_api {
    unsigned version_match;
    unsigned runtime_proven; /* Always zero after binding; not a release gate. */
    const char *(*vi_version)(void);
    const char *(*venc_version)(void);
    const char *(*mem_version)(void);
    const char *(*app_video_version)(void);
    /* Exact target consumes both arguments; older reference code omitted the
     * second. Shipped runtime allocator state confirmed the stock pair(0,0). */
    int (*mem_init)(uint32_t mode, uint32_t policy);
    void (*mem_exit)(void);
    int (*dma_pool_create)(int module, uint32_t bytes);
    int (*dma_pool_activate)(void);
    int (*dma_pool_exit)(void);
    int (*vi_open)(uint32_t device);
    int (*vi_close)(uint32_t device);
    int (*vi_load_sensor_config)(uint32_t device, const char *path);
    int (*vi_get_sensor_resolution)(uint32_t device, uint32_t dimensions[2]);
    int (*vi_get_device_attr)(uint32_t device, struct anyka_vi_device_attr *attr);
    int (*vi_set_device_attr)(uint32_t device, const struct anyka_vi_device_attr *attr);
    int (*vi_get_channel_attr)(uint32_t channel, struct anyka_vi_channel_attr *attr);
    int (*vi_set_channel_attr)(uint32_t channel, const struct anyka_vi_channel_attr *attr);
    int (*vi_enable_device)(uint32_t device);
    int (*vi_disable_device)(uint32_t device);
    int (*vi_enable_channel)(uint32_t channel);
    int (*vi_disable_channel)(uint32_t channel);
    int (*venc_open)(const struct anyka_encode_param *param, int32_t *handle);
    int (*venc_close)(int32_t handle);
    int (*venc_release_stream)(int32_t handle, struct anyka_video_stream *stream);
    int (*video_bind_channel)(struct anyka_channel *source, struct anyka_channel *destination,
                              const struct anyka_channel_attr *attr);
    int (*video_unbind_channel)(struct anyka_channel *source, struct anyka_channel *destination);
    int (*video_set_channel_attr)(struct anyka_channel *destination,
                                  const struct anyka_channel_attr *attr);
    int (*video_set_destination_active)(struct anyka_channel *destination, uint32_t active);
    int (*video_get_stream)(struct anyka_channel *destination, struct anyka_video_stream *stream);
};

/* Zero leaves a complete version-matched table; negative errno leaves it zero.
 * A successful bind is not permission to open a second media owner.
 */
int anyka_sdk_bind(struct anyka_sdk_api *, anyka_symbol_lookup, void *, char *error, size_t capacity);
int anyka_sdk_bind_default(struct anyka_sdk_api *, char *error, size_t capacity);

#endif
