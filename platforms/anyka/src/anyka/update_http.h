#ifndef ANYKA_UPDATE_HTTP_H
#define ANYKA_UPDATE_HTTP_H
#include "../native/control/http.h"
struct anyka_update_http;
struct anyka_update_http_options {
 const char *installer,*runtime_directory;
 /* Set only by genuine APP boot integration, never a RAM preview flag. The
  * installer independently rechecks every physical precondition. */
 int app_root_boot;
};
int anyka_update_http_open(struct anyka_update_http **,const struct anyka_update_http_options *);
void anyka_update_http_backend(void *,const struct control_request *,struct control_reply *);
/* Invoke from management executor after actual liveness checks; not media
 * callback. Publishes health then confirms through trusted installer. */
int anyka_update_http_health(struct anyka_update_http *,int media,int web,int network,int app_root_boot);
#endif
