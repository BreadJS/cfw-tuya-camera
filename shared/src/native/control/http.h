#ifndef CAMERA_CONTROL_HTTP_H
#define CAMERA_CONTROL_HTTP_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
struct control_body {
    void *opaque;
    uint64_t length;
    ssize_t (*read)(void *,void *,size_t);
};
struct control_request {
    const char *method,*path,*content_type;
    struct control_body body;
};
struct control_reply { unsigned status; char json[8192]; };
typedef void (*control_backend)(void *,const struct control_request *,struct control_reply *);
struct control_http_config {
    const char *bind_address,*credentials;
    unsigned port;
    void *opaque;
    control_backend backend;
    /* 0 transfers fd ownership; negative leaves it with HTTP. */
    int (*preview)(void *,int fd);
};
/* Separate bounded worker; HTTP never runs in the media event loop. Config
 * strings and backend context must outlive the service. No shell execution. */
int control_http_start(const struct control_http_config *);
/* Blocking entry point for host test harnesses; same production parser. */
int control_http_run(const struct control_http_config *);
/* Form helpers: missing=0, found=1, malformed/duplicate/overflow=-1. */
int control_form(const char *,const char *,char *,size_t);
int control_body_text(const struct control_body *,char *,size_t);
/* JSON string escaping, including surrounding quotes; -1 on overflow. */
int control_json_string(char *,size_t,const char *);
#endif
