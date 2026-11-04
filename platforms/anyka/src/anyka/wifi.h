/* SPDX-License-Identifier: MIT */
#ifndef CAMERA_ANYKA_WIFI_H
#define CAMERA_ANYKA_WIFI_H
#include "../native/control/http.h"
struct anyka_network;
/* Local files only; does not start a daemon or change an interface. Directory
 * arguments are private, trusted paths, not values accepted from HTTP. */
int anyka_network_open(struct anyka_network **,const char *config_dir,const char *runtime_dir);
void anyka_network_http(void *,const struct control_request *,struct control_reply *);
#endif
