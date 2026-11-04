/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sdk_api.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failure(char *error, size_t capacity, int rc, const char *kind, const char *name)
{
    if (error && capacity) snprintf(error, capacity, "%s: %s", kind, name);
    return rc;
}

int anyka_sdk_bind(struct anyka_sdk_api *api, anyka_symbol_lookup lookup, void *opaque,
                    char *error, size_t capacity)
{
    if (!api) return failure(error, capacity, -EINVAL, "invalid argument", "api");
    memset(api, 0, sizeof *api);
    if (!lookup) return failure(error, capacity, -EINVAL, "invalid argument", "lookup");
    struct anyka_sdk_api proposed = {0};
    void *symbol;
    /* POSIX dynamic-symbol conversion without aliasing the function pointer. */
#define BIND(member, name) do { \
        _Static_assert(sizeof proposed.member == sizeof symbol, "POSIX symbol size"); \
        symbol = lookup(opaque, name); \
        if (!symbol) return failure(error, capacity, -ENOENT, "missing SDK symbol", name); \
        memcpy(&proposed.member, &symbol, sizeof symbol); \
    } while (0)
    BIND(vi_version, "ak_vi_get_version");
    BIND(venc_version, "ak_venc_get_version");
    BIND(mem_version, "ak_mem_get_version");
    BIND(app_video_version, "ak_app_video_get_version");
    const char *actual;
#define VERSION(member, expected) do { \
        actual = proposed.member(); \
        if (!actual || strcmp(actual, expected)) \
            return failure(error, capacity, -EPROTO, "SDK version mismatch", expected); \
    } while (0)
    VERSION(vi_version, "libplat_vi V3.0.02");
    VERSION(venc_version, "libmpi_venc V1.0.29");
    VERSION(mem_version, "libplat_mem V1.3.03");
    VERSION(app_video_version, "libapp_video V1.0.07");
#undef VERSION
    BIND(mem_init, "ak_mem_init");
    BIND(mem_exit, "ak_mem_exit");
    BIND(dma_pool_create, "ak_mem_dma_pool_create");
    BIND(dma_pool_activate, "ak_mem_dma_pool_activate");
    BIND(dma_pool_exit, "ak_mem_dma_pool_exit");
    BIND(vi_open, "ak_vi_open");
    BIND(vi_close, "ak_vi_close");
    BIND(vi_load_sensor_config, "ak_vi_load_sensor_cfg");
    BIND(vi_get_sensor_resolution, "ak_vi_get_sensor_resolution");
    BIND(vi_get_device_attr, "ak_vi_get_dev_attr");
    BIND(vi_set_device_attr, "ak_vi_set_dev_attr");
    BIND(vi_get_channel_attr, "ak_vi_get_chn_attr");
    BIND(vi_set_channel_attr, "ak_vi_set_chn_attr");
    BIND(vi_enable_device, "ak_vi_enable_dev");
    BIND(vi_disable_device, "ak_vi_disable_dev");
    BIND(vi_enable_channel, "ak_vi_enable_chn");
    BIND(vi_disable_channel, "ak_vi_disable_chn");
    BIND(venc_open, "ak_venc_open");
    BIND(venc_close, "ak_venc_close");
    BIND(venc_release_stream, "ak_venc_release_stream");
    BIND(video_bind_channel, "ak_app_video_bind_chn");
    BIND(video_unbind_channel, "ak_app_video_unbind_chn");
    BIND(video_set_channel_attr, "ak_app_video_set_chn_attr");
    BIND(video_set_destination_active, "ak_app_video_set_dst_chn_active");
    BIND(video_get_stream, "ak_app_video_venc_get_stream");
#undef BIND
    proposed.version_match = 1;
    *api = proposed;
    if (error && capacity) error[0] = 0;
    return 0;
}

static void *default_lookup(void *unused, const char *name)
{
    (void)unused;
    return dlsym(RTLD_DEFAULT, name);
}

int anyka_sdk_bind_default(struct anyka_sdk_api *api, char *error, size_t capacity)
{
    return anyka_sdk_bind(api, default_lookup, NULL, error, capacity);
}
