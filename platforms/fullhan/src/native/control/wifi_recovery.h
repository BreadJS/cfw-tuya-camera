#ifndef CAMERA_WIFI_RECOVERY_H
#define CAMERA_WIFI_RECOVERY_H
#include <stddef.h>
#include <stdint.h>
struct camera_wifi_recovery;
int camera_wifi_recovery_open(struct camera_wifi_recovery **,int private_directory);
/* Caller serializes operations. A dedicated network worker calls tick. */
int camera_wifi_recovery_tick(struct camera_wifi_recovery *,uint64_t now,int connected,unsigned channel,int pending);
int camera_wifi_recovery_request(struct camera_wifi_recovery *,int enable);
int camera_wifi_recovery_stop(struct camera_wifi_recovery *);
int camera_wifi_recovery_active(struct camera_wifi_recovery *);
int camera_wifi_recovery_json(struct camera_wifi_recovery *,char *,size_t,int include_password);
#endif
