#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/random.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
static const char *mode;
static unsigned accepts,random_calls,allocations,nodelays,faults,admissions;
static int injected_accept4(int,struct sockaddr *,socklen_t *,int);
static int injected_setsockopt(int,int,int,const void *,socklen_t);
static ssize_t injected_getrandom(void *,size_t,unsigned);
static void *injected_malloc(size_t);
static int injected_listen(int,int);
#define accept4 injected_accept4
#define setsockopt injected_setsockopt
#define getrandom injected_getrandom
#define malloc injected_malloc
#define listen injected_listen
#include "../camera_rtsp.c"
#undef accept4
#undef setsockopt
#undef getrandom
#undef malloc
#undef listen
static int injected_listen(int fd,int backlog){int rc=listen(fd,backlog);if(!rc){puts("LISTEN");fflush(stdout);}return rc;}
static int injected_accept4(int fd,struct sockaddr *a,socklen_t *n,int flags){
 admissions++;
 if(accepts==1&&!strcmp(mode,"pressure")&&faults<3){faults++;errno=EMFILE;return -1;}
 int accepted=accept4(fd,a,n,flags);if(accepted<0)return accepted;
 accepts++;
 if(accepts==2&&strncmp(mode,"accept:",7)==0){close(accepted);errno=atoi(mode+7);faults++;return -1;}
 return accepted;
}
static int injected_setsockopt(int fd,int level,int option,const void *value,socklen_t size){
 if(level==IPPROTO_TCP&&option==TCP_NODELAY&&++nodelays==2&&!strcmp(mode,"nodelay")){errno=ENOPROTOOPT;faults++;return -1;}
 return setsockopt(fd,level,option,value,size);
}
static ssize_t injected_getrandom(void *p,size_t n,unsigned flags){
 if(++random_calls==2&&!strncmp(mode,"random:",7)){errno=atoi(mode+7);faults++;return -1;}
 return getrandom(p,n,flags);
}
static void *injected_malloc(size_t size){if(++allocations==2&&!strcmp(mode,"malloc")){errno=ENOMEM;faults++;return NULL;}return malloc(size);}
static unsigned provider_calls,frames;
static uint64_t epoch;
static int video(void *unused,uint64_t now,struct camera_media_frame *out){
 (void)unused;provider_calls++;
 if(now<epoch+(uint64_t)frames*20000)return 0;
 static const uint8_t au[]={0,0,0,1,0x65,0x88,0x80};
 *out=(struct camera_media_frame){au,sizeof au,epoch+(uint64_t)frames*20000,1,0,0};frames++;return 1;
}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 mode=argv[1];
 const uint8_t sps[]={0x67,0x42,0,0x1e},pps[]={0x68,0x80};
 struct camera_media_provider provider={.description={.sps=sps,.sps_size=sizeof sps,.pps=pps,.pps_size=sizeof pps},.video=video};
 struct camera_rtsp_config cfg={"127.0.0.1",(unsigned)atoi(argv[2]),"/main","fault:test",4,2000};
 epoch=camera_monotonic_us();int rc=camera_rtsp_run(&cfg,&provider);
 printf("DONE rc=%d accepts=%u faults=%u calls=%u frames=%u admissions=%u\n",rc,accepts,faults,provider_calls,frames,admissions);return rc?1:0;
}
