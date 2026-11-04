/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_TRIAL_GUARD_H
#define CAMERA_ANYKA_TRIAL_GUARD_H
#include <stdint.h>
int anyka_trial_safe_sdk_host(void);
int anyka_trial_exclusive_owner(void);
int anyka_trial_dma_budget(uint32_t vi_pool, uint32_t venc_pool);
int anyka_trial_readable(uint32_t address, uint32_t bytes);
#endif
