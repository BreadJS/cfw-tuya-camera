#define _GNU_SOURCE
#include "../../src/anyka/picture_executor.c"
#include <assert.h>
static unsigned opened,read_count,idr,published;
int anyka_encoder_control_open(struct anyka_encoder_control **out,unsigned channel)
{assert(channel==0);opened++;*out=(void*)1;return 0;}
int anyka_encoder_control_read(struct anyka_encoder_control *e,struct anyka_encoder_snapshot *s)
{assert(e);read_count++;memset(s,0,sizeof *s);return 0;}
int anyka_encoder_request_idr(struct anyka_encoder_control *e){assert(e);idr++;return 0;}
void anyka_management_encoder_state(struct anyka_management *m,const struct anyka_encoder_snapshot *s,int ready)
{assert(m&&s&&ready);published++;}
int main(void)
{
 struct anyka_executor e={.management=(void*)1};
 for(unsigned i=0;i<100;i++)camera_request_keyframe();
 assert(!opened&&!idr);
 encoder_tick(&e,0,0,0);assert(!opened&&!idr);
 encoder_tick(&e,100,1,0);assert(opened==1&&idr==1&&read_count==1);
 for(unsigned i=0;i<100;i++)camera_request_keyframe();
 encoder_tick(&e,1099,1,0);assert(idr==1);
 encoder_tick(&e,1100,1,0);assert(idr==2&&read_count==2);
 encoder_tick(&e,2100,1,0);assert(idr==2&&opened==1&&published==3);
 puts("PASS encoder executor: callback only queues; owner binds once, coalesces IDR to one/second and polls readback");
}
