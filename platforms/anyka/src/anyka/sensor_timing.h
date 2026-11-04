/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_SENSOR_TIMING_H
#define CAMERA_ANYKA_SENSOR_TIMING_H
/* Exact shipped SDK ABI. The scalar getter returns FPS itself, unlike the
 * similarly named ak_vpss_get_sensor_fps output-pointer wrapper. */
struct anyka_sensor_timing {
    int (*sensor_get)(int device);
    int (*sensor_set)(int device, void *fps);
    int (*levels_get)(int device, void *levels);
    int (*levels_set)(int device, void *levels);
};
int anyka_sensor_timing_bind(struct anyka_sensor_timing *);
/* Only the existing exclusive SDK owner may call this, after loading its
 * profile. It opens no device and never initializes a second media owner. */
int anyka_sensor_timing_apply(struct anyka_sensor_timing *, unsigned fps);
#endif
