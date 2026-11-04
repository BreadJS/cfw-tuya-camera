#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include "../../src/anyka/board_control.h"
static struct anyka_motor_status axes[2]={{0,2048,600,52,4096,4096,0,1},{0,1024,600,52,4096,2048,0,1}};
static unsigned moves,stops;static int32_t amount;static int fail_move,fail_stop;
static int mock_ioctl(int fd,unsigned long request,...){
 va_list args;va_start(args,request);void *arg=request==0x40046d42u?NULL:va_arg(args,void*);va_end(args);assert(fd==0||fd==1);
 if(request==0x40046d43u){*(struct anyka_motor_status*)arg=axes[fd];return 0;}
 if(request==0x40046d42u){stops++;if(fail_stop){errno=EIO;return -1;}axes[fd].busy=0;return 0;}
 assert(request==0x40046d40u);moves++;amount=*(int32_t*)arg;if(fail_move){errno=EIO;return -1;}axes[fd].busy=1;return 0;
}
#define ioctl mock_ioctl
#include "../../src/anyka/board_control.c"
int main(void){
 struct anyka_board b={.motor={0,1},.lock=-1};
 assert(!anyka_board_hold(&b,2,0));assert(moves==1&&stops==2&&amount==2048&&b.hold_direction==2);
 unsigned initial_stops=stops;uint64_t deadline=b.stop_at;
 for(unsigned i=0;i<20;i++)assert(!anyka_board_hold(&b,2,0));
 assert(moves==1&&stops==initial_stops&&b.stop_at>=deadline);
 assert(!anyka_board_stop(&b)&&!b.hold_direction&&!b.stop_at&&!axes[0].busy);
 assert(!anyka_board_hold(&b,1,0)&&amount==-2048);unsigned previous=moves;
 b.stop_at=milliseconds()-1;assert(!anyka_board_hold(&b,1,0)&&moves==previous+1);
 assert(!anyka_board_hold(&b,1,1)&&amount==2048&&b.hold_rotated==1);
 assert(!anyka_board_hold(&b,3,0)&&amount==1024&&!axes[0].busy&&axes[1].busy);
 assert(!anyka_board_hold(&b,4,0)&&amount==-1024);
 previous=stops;deadline=b.stop_at;anyka_board_tick(&b,deadline-1);assert(stops==previous);anyka_board_tick(&b,deadline);assert(stops==previous+2&&!b.stop_at&&!b.hold_direction);anyka_board_tick(&b,deadline+1000);assert(stops==previous+2);
 axes[0].position=4090;assert(!anyka_board_hold(&b,2,0)&&amount==6);assert(!anyka_board_stop(&b));
 axes[0].position=4096;previous=moves;assert(!anyka_board_hold(&b,2,0)&&moves==previous);assert(!anyka_board_hold(&b,2,0)&&moves==previous);
 assert(!anyka_board_stop(&b));axes[0].total_steps=2000;axes[0].position=1000;assert(anyka_board_hold(&b,2,0)==-EPROTO);axes[0].total_steps=4096;
 axes[0].steps_per_second=1200;assert(anyka_board_hold(&b,2,0)==-EPROTO);axes[0].steps_per_second=600;
 fail_move=1;assert(anyka_board_hold(&b,2,0)==-EIO&&!b.hold_direction&&!b.stop_at);fail_move=0;
 fail_stop=1;assert(anyka_board_stop(&b)==-EIO&&b.stop_at&&!b.hold_direction);fail_stop=0;anyka_board_tick(&b,b.stop_at);assert(!b.stop_at);
 assert(anyka_board_hold(&b,5,0)==-EINVAL&&anyka_board_hold(&b,2,2)==-EINVAL);
 puts("PASS smooth PTZ: one finite normal-boundary MOVE, zero timer/STOP ioctls on heartbeats, rotation/reversal, lease expiry, endpoint no-restart and unknown-range/failure refusal");
}
