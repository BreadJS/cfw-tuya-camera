/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "board_control.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#define MOTOR_MOVE 0x40046d40u
#define MOTOR_STOP 0x40046d42u
#define MOTOR_STATUS 0x40046d43u
struct anyka_board {int motor[2],lock;uint64_t stop_at;unsigned hold_direction;int hold_rotated;};
static uint64_t milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u;}
static int write_value(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;size_t size=strlen(text);ssize_t n;do{n=write(fd,text,size);}while(n<0&&errno==EINTR);int rc=n==(ssize_t)size?0:-(n<0?errno:EIO);close(fd);return rc;}
static int read_int(const char *path,int *value){char buf[64],*end;int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;ssize_t n=read(fd,buf,sizeof buf-1);int rc=0;if(n<=0)rc=-(n<0?errno:EIO);else{buf[n]=0;errno=0;long v=strtol(buf,&end,10);if(errno||end==buf||v<0||v>8192)rc=-ERANGE;else *value=(int)v;}close(fd);return rc;}
static int gpio_ready(unsigned pin){char path[128],number[16],direction[16];snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/direction",pin);int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0&&errno==ENOENT){snprintf(number,sizeof number,"%u",pin);int rc=write_value("/sys/class/gpio/export",number);if(rc&&rc!=-EBUSY)return rc;fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);}if(fd<0)return -errno;ssize_t n=read(fd,direction,sizeof direction-1);close(fd);if(n<0)return -errno;direction[n]=0;if(strcmp(direction,"out\n"))return write_value(path,"out");return 0;}
static int gpio_write(unsigned pin,int enabled){char path[128];snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/value",pin);return write_value(path,enabled?"1":"0");}
static int led_write(const char *name,int enabled){char path[160];snprintf(path,sizeof path,"/sys/class/leds/%s/trigger",name);int rc=write_value(path,"none");if(rc)return rc;snprintf(path,sizeof path,"/sys/class/leds/%s/brightness",name);return write_value(path,enabled?"1":"0");}
static int motors_unowned(void){DIR *processes=opendir("/proc");if(!processes)return -errno;struct dirent *p;int rc=0;while((p=readdir(processes))){char *end;long pid=strtol(p->d_name,&end,10);if(!*p->d_name||*end||pid<=0||pid==(long)getpid())continue;char path[128];snprintf(path,sizeof path,"/proc/%ld/fd",pid);DIR *fds=opendir(path);if(!fds){if(errno==ENOENT||errno==ESRCH)continue;rc=-errno;break;}struct dirent *f;while((f=readdir(fds))){if(f->d_name[0]=='.')continue;char link[256],target[256];int n=snprintf(link,sizeof link,"%s/%s",path,f->d_name);if(n<0||(size_t)n>=sizeof link){rc=-ENAMETOOLONG;break;}ssize_t count=readlink(link,target,sizeof target-1);if(count<0){if(errno==ENOENT)continue;rc=-errno;break;}target[count]=0;if(!strcmp(target,"/dev/motor0")||!strcmp(target,"/dev/motor1")){rc=-EBUSY;break;}}closedir(fds);if(rc)break;}closedir(processes);return rc;}
static int motor_status(int fd,struct anyka_motor_status *out){if(fd<0)return -ENODEV;if(ioctl(fd,MOTOR_STATUS,out)<0)return -errno;if(out->steps_per_circle!=4096||out->total_steps<=0||out->total_steps>8192||out->boundary_steps<0||out->boundary_steps>=out->total_steps/2||out->position<0||out->position>out->total_steps)return -EPROTO;return 0;}
int anyka_board_open(struct anyka_board **out,int enable_motors){if(!out||*out)return -EINVAL;struct anyka_board *b=calloc(1,sizeof *b);if(!b)return -ENOMEM;b->motor[0]=b->motor[1]=b->lock=-1;int rc=0;if(CAMERA_HAS_IRCUT){rc=gpio_ready(CAMERA_IRCUT_SELECT_PIN);if(!rc)rc=gpio_ready(CAMERA_IRCUT_ENABLE_PIN);}if(!rc&&CAMERA_HAS_IR_LED&&!CAMERA_IR_LED_LED_CLASS)rc=gpio_ready(CAMERA_IR_LED_PIN);if(!rc&&CAMERA_HAS_STATUS_LED&&!CAMERA_STATUS_LED_LED_CLASS)rc=gpio_ready(CAMERA_STATUS_LED_PIN);if(!CAMERA_HAS_PTZ)enable_motors=0;if(rc){free(b);return rc;}if(enable_motors){b->lock=open("/tmp/camera-motors.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(b->lock<0)rc=-errno;else if(flock(b->lock,LOCK_EX|LOCK_NB))rc=-errno;if(!rc)rc=motors_unowned();for(unsigned i=0;!rc&&i<2;i++){char path[32];snprintf(path,sizeof path,"%s",i?CAMERA_PTZ_TILT_DEVICE:CAMERA_PTZ_PAN_DEVICE);b->motor[i]=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(b->motor[i]<0)rc=-errno;else{struct anyka_motor_status status;rc=motor_status(b->motor[i],&status);}}if(rc){anyka_board_close(b);return rc;}}*out=b;return 0;}
int anyka_board_status(struct anyka_board *b,struct anyka_board_status *s){if(!b||!s)return -EINVAL;memset(s,0,sizeof *s);int rc=0;s->day_mode=1;if(CAMERA_HAS_IRCUT){char path[128];snprintf(path,sizeof path,CAMERA_GPIO_DIR_FORMAT "/value",CAMERA_IRCUT_SELECT_PIN);rc=read_int(path,&s->day_mode);if(!rc)s->day_mode=s->day_mode==CAMERA_IRCUT_DAY_LEVEL;}if(!rc&&CAMERA_HAS_IR_LED){rc=read_int(CAMERA_IR_LED_LED_CLASS?"/sys/class/leds/" CAMERA_IR_LED_NAME "/brightness":CAMERA_IR_LED_VALUE,&s->ir_led);if(!rc&&!CAMERA_IR_LED_LED_CLASS)s->ir_led=s->ir_led==CAMERA_IR_LED_ON_LEVEL;}if(!rc&&CAMERA_HAS_STATUS_LED){rc=read_int(CAMERA_STATUS_LED_LED_CLASS?"/sys/class/leds/" CAMERA_STATUS_LED_NAME "/brightness":CAMERA_STATUS_LED_VALUE,&s->status_led);if(!rc&&!CAMERA_STATUS_LED_LED_CLASS)s->status_led=s->status_led==CAMERA_STATUS_LED_ON_LEVEL;}s->motors_available=b->motor[0]>=0&&b->motor[1]>=0;if(!rc&&s->motors_available)rc=motor_status(b->motor[0],&s->pan);if(!rc&&s->motors_available)rc=motor_status(b->motor[1],&s->tilt);return rc;}
int anyka_board_day(struct anyka_board *b,int day){if(!b||(day!=0&&day!=1))return -EINVAL;if(!CAMERA_HAS_IRCUT)return -ENOTSUP;int rc=gpio_write(CAMERA_IRCUT_ENABLE_PIN,CAMERA_IRCUT_ENABLE_LEVEL);if(rc)return rc;struct timespec wait={0,(long)CAMERA_IRCUT_SETTLE_US*1000};while(nanosleep(&wait,&wait)&&errno==EINTR){}return gpio_write(CAMERA_IRCUT_SELECT_PIN,day?CAMERA_IRCUT_DAY_LEVEL:!CAMERA_IRCUT_DAY_LEVEL);}
int anyka_board_ir(struct anyka_board *b,int on){if(!b||(on!=0&&on!=1))return -EINVAL;if(!CAMERA_HAS_IR_LED)return -ENOTSUP;return CAMERA_IR_LED_LED_CLASS?led_write(CAMERA_IR_LED_NAME,on):gpio_write(CAMERA_IR_LED_PIN,on?CAMERA_IR_LED_ON_LEVEL:!CAMERA_IR_LED_ON_LEVEL);}
int anyka_board_led(struct anyka_board *b,int on){if(!b||(on!=0&&on!=1))return -EINVAL;if(!CAMERA_HAS_STATUS_LED)return -ENOTSUP;return CAMERA_STATUS_LED_LED_CLASS?led_write(CAMERA_STATUS_LED_NAME,on):gpio_write(CAMERA_STATUS_LED_PIN,on?CAMERA_STATUS_LED_ON_LEVEL:!CAMERA_STATUS_LED_ON_LEVEL);}
int anyka_board_stop(struct anyka_board *b){if(!b)return -EINVAL;int rc=0;for(unsigned i=0;i<2;i++)if(b->motor[i]>=0&&ioctl(b->motor[i],MOTOR_STOP,0)<0)rc=-errno;b->stop_at=rc?milliseconds()+20:0;b->hold_direction=0;return rc;}
int anyka_board_move(struct anyka_board *b,unsigned direction,unsigned steps,int rotated){if(!b||direction>4||steps>128||(rotated!=0&&rotated!=1))return -EINVAL;if(!direction)return anyka_board_stop(b);if(!steps)return -EINVAL;unsigned axis=direction>=3;struct anyka_motor_status status;int rc=motor_status(b->motor[axis],&status);if(rc)return rc;int32_t amount=(int32_t)steps;if(direction==1||direction==4)amount=-amount;if((axis?CAMERA_PTZ_TILT_INVERT:CAMERA_PTZ_PAN_INVERT))amount=-amount;if(rotated)amount=-amount;int64_t destination=(int64_t)status.position+amount;if(destination<status.boundary_steps||destination>status.total_steps-status.boundary_steps)return -ERANGE;rc=anyka_board_stop(b);if(rc)return rc;if(ioctl(b->motor[axis],MOTOR_MOVE,&amount)<0)return -errno;b->stop_at=milliseconds()+500;return 0;}
/* A hold starts one finite normal-boundary move. Renewals change only the
 * lease: repeating MOVE restarts the driver's timer and repeating STOP causes
 * visible acceleration steps. Only this executor owns these descriptors. */
int anyka_board_hold(struct anyka_board *b,unsigned direction,int rotated)
{
    if(!b||direction>4||(rotated!=0&&rotated!=1))return -EINVAL;
    if(!direction)return anyka_board_stop(b);
    uint64_t now=milliseconds();
    if(b->hold_direction==direction&&b->hold_rotated==rotated&&b->stop_at>now){
        b->stop_at=now+CAMERA_PTZ_LEASE_MS;return 0;
    }
    int rc=anyka_board_stop(b);if(rc)return rc;
    /* Reversals must finish decelerating before calculating remaining travel.
     * Never restart a timer while the previous axis is still moving. */
    struct anyka_motor_status status[2];uint64_t wait_until=milliseconds()+500;
    for(;;){
        for(unsigned i=0;i<2;i++){rc=motor_status(b->motor[i],&status[i]);if(rc)return rc;}
        if(!status[0].busy&&!status[1].busy)break;
        if(milliseconds()>=wait_until)return -ETIMEDOUT;
        usleep(10000);
    }
    for(unsigned i=0;i<2;i++)if(status[i].total_steps!=(i?CAMERA_PTZ_TILT_STEPS:CAMERA_PTZ_PAN_STEPS)||status[i].boundary_steps!=0||status[i].steps_per_second!=CAMERA_PTZ_SPEED)return -EPROTO;
    unsigned axis=direction>=3;
    int sign=(direction==1||direction==4)?-1:1;if((axis?CAMERA_PTZ_TILT_INVERT:CAMERA_PTZ_PAN_INVERT))sign=-sign;if(rotated)sign=-sign;
    int32_t amount=sign>0?status[axis].total_steps-status[axis].boundary_steps-status[axis].position:
                         status[axis].boundary_steps-status[axis].position;
    if(amount&&ioctl(b->motor[axis],MOTOR_MOVE,&amount)<0){rc=-errno;(void)anyka_board_stop(b);return rc;}
    b->hold_direction=direction;b->hold_rotated=rotated;b->stop_at=milliseconds()+CAMERA_PTZ_LEASE_MS;
    return 0;
}
void anyka_board_tick(struct anyka_board *b,uint64_t now){if(b&&b->stop_at&&now>=b->stop_at)(void)anyka_board_stop(b);}
void anyka_board_close(struct anyka_board *b){if(!b)return;(void)anyka_board_stop(b);for(unsigned i=0;i<2;i++)if(b->motor[i]>=0)close(b->motor[i]);if(b->lock>=0)close(b->lock);free(b);}

int anyka_board_calibrate(struct anyka_board *b)
{
    /* One board/owner per process. Preserve a failure even if someone closes
     * and reopens a context: configured totals alone cannot prove that an
     * interrupted homing sequence established a real mechanical position. */
    static int failed;
    if(!b)return -EINVAL;
    if(failed)return -EALREADY;
    if(b->motor[0]<0||b->motor[1]<0)return -ENODEV;
    const int32_t total[2]={CAMERA_PTZ_PAN_STEPS,CAMERA_PTZ_TILT_STEPS},fresh_total[2]={2000,2000};
    struct anyka_motor_status status[2];
    int rc=0,configured=1,fresh=1;
    if(b->stop_at)rc=-EBUSY;
    for(unsigned i=0;!rc&&i<2;i++){
        rc=motor_status(b->motor[i],&status[i]);
        if(rc)break;
        if(status[i].busy){rc=-EBUSY;break;}
        configured&=status[i].steps_per_circle==4096&&status[i].total_steps==total[i]&&status[i].boundary_steps==0;
        fresh&=status[i].steps_per_circle==4096&&status[i].total_steps==fresh_total[i]&&status[i].boundary_steps==50&&status[i].position==0;
    }
    if(!rc&&configured)return 0;
    if(!rc&&!fresh)rc=-EPROTO;
    for(unsigned i=0;!rc&&i<2;i++){
        int32_t config[5]={0,CAMERA_PTZ_SPEED,CAMERA_PTZ_CIRCLE_STEPS,total[i],0};
        struct {int32_t total,position;} result={-1,-1};
        _Static_assert(sizeof config==20,"motor configuration ABI");
        _Static_assert(sizeof result==8,"motor calibration output ABI");
        if(ioctl(b->motor[i],0x40046d00u,config)<0){rc=-errno;break;}
        uint64_t start=milliseconds();
        if(ioctl(b->motor[i],0x40046d60u,&result)<0){rc=-errno;break;}
        /* Post-return budget only. TASK_UNINTERRUPTIBLE kernel waits cannot
         * be cancelled by a userspace timer; never pretend this is a deadline
         * that stops physical movement at120seconds. */
        if(milliseconds()-start>120000u){rc=-ETIMEDOUT;break;}
        rc=motor_status(b->motor[i],&status[i]);
        if(!rc&&(result.total!=total[i]||result.position!=total[i]/2||
                 status[i].busy||status[i].position!=result.position||
                 status[i].steps_per_second!=CAMERA_PTZ_SPEED||status[i].steps_per_circle!=4096||
                 status[i].total_steps!=total[i]||status[i].boundary_steps!=0))rc=-EPROTO;
    }
    if(rc){
        failed=1;
        /* Keep both descriptors and the exclusive lock. A STOP error does
         * not erase the primary failure or justify releasing DMA/ownership. */
        (void)anyka_board_stop(b);
    }
    return rc;
}
