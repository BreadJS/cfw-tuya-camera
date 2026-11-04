#define _POSIX_C_SOURCE 200809L
#include "../../src/native/control/http.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static void backend(void *p,const struct control_request *q,struct control_reply *r){(void)p;(void)q;r->status=200;strcpy(r->json,"{\"alive\":true}");}
static void *stream(void *p){int fd=*(int *)p;free(p);struct timespec t={.tv_nsec=150000000};nanosleep(&t,NULL);const char text[]="HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: 13\r\nConnection: close\r\n\r\npreview alive";(void)send(fd,text,sizeof text-1,MSG_NOSIGNAL);close(fd);return NULL;}
static int preview(void *p,int fd){(void)p;int *copy=malloc(sizeof *copy);if(!copy)return -1;*copy=fd;pthread_t thread;if(pthread_create(&thread,NULL,stream,copy)){free(copy);return -1;}pthread_detach(thread);return 0;}
int main(int argc,char **argv){if(argc!=2)return 2;struct control_http_config config={.bind_address="127.0.0.1",.credentials="camera:test-only",.port=(unsigned)atoi(argv[1]),.backend=backend,.preview=preview};return control_http_run(&config)?1:0;}
