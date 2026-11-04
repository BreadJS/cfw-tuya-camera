#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../src/anyka/board_control.h"
static struct anyka_motor_status axes[2];
static unsigned configs,calibrations,stops;
static int failure;
static uint64_t now_ms;
static int clock_mock(clockid_t clock,struct timespec *out)
{assert(clock==CLOCK_MONOTONIC);out->tv_sec=(time_t)(now_ms/1000);out->tv_nsec=(long)(now_ms%1000)*1000000;return 0;}
static int ioctl_mock(int fd,unsigned long request,...)
{
    assert(fd==0||fd==1);
    va_list args;va_start(args,request);
    void *arg=request==0x40046d42u?NULL:va_arg(args,void *);va_end(args);
    if(request==0x40046d43u){memcpy(arg,&axes[fd],32);return 0;}
    if(request==0x40046d42u){stops++;return 0;}
    if(request==0x40046d00u){
        const int32_t *words=arg;configs++;
        assert(words[0]==0&&words[1]==600&&words[2]==4096&&words[3]==(fd?2048:4096)&&words[4]==0);
        if(failure==1){errno=EIO;return -1;}
        axes[fd].position=words[0];axes[fd].steps_per_second=words[1];axes[fd].steps_per_circle=words[2];axes[fd].total_steps=words[3];axes[fd].boundary_steps=words[4];return 0;
    }
    assert(request==0x40046d60u); /* No bypass, cruise, end-stop or move calls. */
    calibrations++;
    if(failure==2){errno=EIO;return -1;}
    int32_t result[2]={axes[fd].total_steps,axes[fd].total_steps/2};
    axes[fd].position=result[1];axes[fd].busy=0;
    if(failure==3)result[0]++;
    if(failure==4)axes[fd].busy=1;
    if(failure==5)now_ms+=120001;else now_ms+=2000;
    memcpy(arg,result,sizeof result);return 0;
}
#define ioctl ioctl_mock
#define clock_gettime clock_mock
#include "../../src/anyka/board_control.c"
static void run(unsigned which)
{
    struct anyka_board b={.motor={0,1},.lock=-1};
    axes[0]=(struct anyka_motor_status){0,0,500,43,4096,2000,50,1};
    axes[1]=(struct anyka_motor_status){0,0,500,43,4096,2000,50,1};
    if(which==0){
        axes[0].total_steps=4096;axes[0].boundary_steps=0;axes[0].position=2355;
        axes[1].total_steps=2048;axes[1].boundary_steps=0;axes[1].position=808;
        assert(!anyka_board_calibrate(&b));assert(!configs&&!calibrations&&!stops);return;
    }
    if(which==1){
        assert(!anyka_board_calibrate(&b));assert(configs==2&&calibrations==2&&!stops);
        assert(axes[0].position==2048&&axes[1].position==1024);return;
    }
    if(which==9){axes[0].total_steps=2500;axes[1].total_steps=600;assert(anyka_board_calibrate(&b)==-EPROTO);assert(!configs&&!calibrations&&stops==2);}
    else if(which==2){axes[1].position=20;assert(anyka_board_calibrate(&b)==-EPROTO);assert(!configs&&!calibrations&&stops==2);}
    else if(which==3){axes[0].busy=1;assert(anyka_board_calibrate(&b)==-EBUSY);assert(!configs&&!calibrations&&stops==2);}
    else {
        failure=(int)which-3;
        int expected=failure==5?-ETIMEDOUT:failure>=3?-EPROTO:-EIO;
        assert(anyka_board_calibrate(&b)==expected);assert(configs==1&&calibrations==(unsigned)(failure!=1)&&stops==2);
    }
    assert(b.motor[0]==0&&b.motor[1]==1); /* No ownership/descriptor handoff. */
    unsigned before=configs;
    assert(anyka_board_calibrate(&b)==-EALREADY);assert(configs==before);
}
int main(void)
{
    for(unsigned i=0;i<10;i++){
        pid_t pid=fork();assert(pid>=0);
        if(!pid){run(i);_exit(0);}
        int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    puts("PASS optional PTZ calibration: exact ABI/config, fresh-only homing, existing pose retained, failure latch, no bypass or unsafe cancellation");
    return 0;
}
