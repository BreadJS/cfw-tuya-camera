#define _GNU_SOURCE
#include "../../src/native/control/update.h"
#include "hooks.h"
#include <sys/resource.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
static const char *helper,*progress,*map,*mode;
static pthread_mutex_t event_mutex=PTHREAD_MUTEX_INITIALIZER;
static long long reboot_ns;static int sync_calls;
char *camera_test_installer(void){return (char *)helper;}
const char *camera_test_progress(void){return progress;}
const char *camera_test_mtd_map(void){return map;}
int camera_test_fstat(int fd,struct stat *s){int rc=fstat(fd,s);if(!rc&&s->st_uid==geteuid())s->st_uid=0;return rc;}
int camera_test_fstatat(int fd,const char *p,struct stat *s,int flags){int rc=fstatat(fd,p,s,flags);if(!rc&&s->st_uid==geteuid())s->st_uid=0;return rc;}
int camera_test_fstatfs(int fd,struct statfs *s){int rc=fstatfs(fd,s);if(!rc)s->f_type=0x01021994;return rc;}
pid_t camera_test_fork(void){if(!strcmp(mode,"fork_fail")){errno=EAGAIN;return -1;}return fork();}
int camera_test_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p){return !strcmp(mode,"thread_fail")?EAGAIN:pthread_create(t,a,f,p);}
void camera_test_sync(void){pthread_mutex_lock(&event_mutex);sync_calls++;pthread_mutex_unlock(&event_mutex);}
int camera_test_reboot(int command){(void)command;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);pthread_mutex_lock(&event_mutex);reboot_ns=(long long)t.tv_sec*1000000000+t.tv_nsec;pthread_mutex_unlock(&event_mutex);errno=EPERM;return -1;}
static void pause_ms(unsigned ms){struct timespec t={ms/1000,(ms%1000)*1000000};while(nanosleep(&t,&t)&&errno==EINTR){}}
static int active(struct control_update *u){pthread_mutex_lock(&u->mutex);int n=u->monitor_active;pthread_mutex_unlock(&u->mutex);return n;}
static ssize_t forbidden_body(void *p,void *out,size_t n){(void)p;(void)out;(void)n;abort();}
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv)
{
    REQUIRE(argc==9);mode=argv[1];helper=argv[5];progress=argv[6];map=argv[7];signal(SIGPIPE,SIG_IGN);
    struct control_update u;memset(&u,0,sizeof u);u.cache_fd=u.progress_directory=-1;char error[256];uint8_t key[32];
    REQUIRE(!ota_read_public_key(argv[2],key,1,error,sizeof error));REQUIRE(!ota_state_open(&u.state,argv[3],argv[4],key,error,sizeof error));
    REQUIRE(!pthread_mutex_init(&u.mutex,NULL));u.initialized=1;setenv("CAMERA_TEST_SECRET","must-not-cross-exec",1);
    struct control_reply reply;
    if(!strcmp(mode,"reset_pending")){
        u.reset_pending=1;REQUIRE(control_update_busy(&u));
        REQUIRE(!control_update_can_install_locked(&u));
        control_update_install(&u,2,argv[8],&reply);REQUIRE(reply.status==409);
        control_update_plan(&u,&reply);REQUIRE(reply.status==409);
        struct control_body body={NULL,OTA_HEADER_SIZE+32,forbidden_body};control_update_upload(&u,&body,&reply);REQUIRE(reply.status==409);
        REQUIRE(!active(&u));REQUIRE(!control_update_close(&u));return 0;
    }
    if(!strcmp(mode,"progress")){
        u.progress_directory=open(progress,O_DIRECTORY|O_RDONLY);char json[8192]="{\"ok\":true}";control_update_install_status_locked(&u,json,sizeof json);puts(json);REQUIRE(!control_update_close(&u));return 0;
    }
    if(!strcmp(mode,"high_fd")){int fd=open("/dev/null",O_RDONLY);REQUIRE(fd>=0&&dup2(fd,120)==120);close(fd);struct rlimit lim;REQUIRE(!getrlimit(RLIMIT_NOFILE,&lim));lim.rlim_cur=96;REQUIRE(!setrlimit(RLIMIT_NOFILE,&lim));}
    uint64_t seq=!strcmp(mode,"stale_sequence")?3:2;
    const char *digest=!strcmp(mode,"stale_digest")?"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000":argv[8];
    control_update_install(&u,seq,digest,&reply);printf("response=%u\n%s\n",reply.status,reply.json);
    if(!strcmp(mode,"fork_fail")||!strcmp(mode,"thread_fail")||!strncmp(mode,"stale_",6)){
        REQUIRE(reply.status==(!strncmp(mode,"stale_",6)?409:503));REQUIRE(!active(&u));REQUIRE(!control_update_close(&u));return 0;
    }
    REQUIRE(reply.status==202);
    int grace_busy=0;
    if(!strcmp(mode,"success")){
        REQUIRE(control_update_close(&u)==-EBUSY);pause_ms(500);REQUIRE(control_update_close(&u)==-EBUSY);grace_busy=1;
    }
    for(int i=0;i<500&&active(&u);i++)pause_ms(10);
    REQUIRE(!active(&u));printf("pristine=%d written=%d grace_busy=%d\n",u.install_pristine,u.install_written,grace_busy);
    if(!strcmp(mode,"retry_pristine")||!strcmp(mode,"retry_dirty")){
        int fd=openat(u.progress_directory,"scenario",O_WRONLY|O_TRUNC);REQUIRE(fd>=0&&write(fd,"10",2)==2);close(fd);
        control_update_install(&u,2,argv[8],&reply);REQUIRE(reply.status==202);for(int i=0;i<500&&active(&u);i++)pause_ms(10);REQUIRE(!active(&u));
    }
    if(!strcmp(mode,"success")){
        pthread_mutex_lock(&event_mutex);printf("reboot_ns=%lld sync_calls=%d\n",reboot_ns,sync_calls);REQUIRE(reboot_ns>0&&sync_calls==1);pthread_mutex_unlock(&event_mutex);
    }else{
        pthread_mutex_lock(&event_mutex);REQUIRE(!reboot_ns&&!sync_calls);pthread_mutex_unlock(&event_mutex);
        control_update_cancel(&u,&reply);printf("cancel=%u\n",reply.status);
    }
    REQUIRE(!control_update_close(&u));return 0;
}
