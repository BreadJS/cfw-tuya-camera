#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include "../../src/anyka/board_control.h"
static struct anyka_motor_status axes[2]={{1,2000,600,52,4096,4096,0,1},{1,800,600,52,4096,2048,0,1}};
static unsigned moves,stops;
static int mock_ioctl(int fd,unsigned long request,...){va_list args;va_start(args,request);void *arg=request==0x40046d42u?NULL:va_arg(args,void *);va_end(args);if(request==0x40046d43u){*(struct anyka_motor_status*)arg=axes[fd];return 0;}if(request==0x40046d42u){stops++;return 0;}assert(request==0x40046d40u);moves++;axes[fd].position+=*(int32_t*)arg;return 0;}
#define ioctl mock_ioctl
#include "../../src/anyka/board_control.c"
int main(void){struct anyka_board b={.motor={0,1},.lock=-1};assert(anyka_board_move(&b,2,16,0)==0&&axes[0].position==2016);assert(stops==2&&moves==1);assert(anyka_board_move(&b,2,16,1)==0&&axes[0].position==2000);assert(anyka_board_move(&b,3,16,0)==0&&axes[1].position==816);assert(anyka_board_move(&b,4,16,0)==0&&axes[1].position==800);assert(anyka_board_move(&b,1,129,0)==-EINVAL);assert(anyka_board_move(&b,7,16,0)==-EINVAL);assert(anyka_board_move(&b,1,16,2)==-EINVAL);axes[0].position=0;assert(anyka_board_move(&b,1,16,0)==-ERANGE);unsigned before=stops;b.stop_at=100;anyka_board_tick(&b,99);assert(stops==before);anyka_board_tick(&b,100);assert(stops==before+2&&b.stop_at==0);axes[0].steps_per_circle=1;assert(anyka_board_move(&b,2,16,0)==-EPROTO);puts("PASS finite PTZ: bounds, rotation, direction, status validation, stop deadline, no calibration/bypass ioctl");return 0;}
