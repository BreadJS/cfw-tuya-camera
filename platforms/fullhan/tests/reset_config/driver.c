#define _GNU_SOURCE
#include "../../src/reset/config_reset.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
static int fault(void *ctx,const char *p){return !strcmp(ctx,p);}
int main(int argc,char **argv)
{
    if(argc!=6)return 2;
    int dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return 2;
    int lock=-1;
    if(!strcmp(argv[4],"borrowed")){
        int state=openat(dir,"update-state",O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(state<0)return 2;
        lock=openat(state,"lock",O_RDWR|O_CLOEXEC|O_NOFOLLOW);close(state);
        if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 2;
    }else if(!strcmp(argv[4],"wrong"))lock=openat(dir,"unrelated",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    char e[160];int rc=!strcmp(argv[1],"request")?camera_config_reset_request(dir,lock,atoi(argv[5]),fault,argv[3],e,sizeof e):camera_config_reset_apply(dir,lock,fault,argv[3],e,sizeof e);
    if(lock>=0){if(fcntl(lock,F_GETFD)<0)return 3;close(lock);}
    printf("rc=%d error=%s\n",rc,e);close(dir);return rc<0?1:0;
}
