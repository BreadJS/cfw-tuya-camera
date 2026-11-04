#ifndef CAMERA_CREDENTIALS_H
#define CAMERA_CREDENTIALS_H
#include <stddef.h>
/* Check the current saved login; role0 web/Telnet, role1 stream. */
int camera_credentials_verify(unsigned role,const char *user,const char *password);
/* Optional for standalone transport fixtures; application links the store. */
int camera_credentials_token(unsigned role,char *out,size_t size,unsigned *generation) __attribute__((weak));
int camera_credentials_open(int directory,const char *initial);
int camera_credentials_user(unsigned role,char *out,size_t size);
int camera_credentials_change(unsigned role,const char *user,const char *password);
#endif
