#include "../../src/anyka/sensor_timing.c"
#include <assert.h>
#include <stdio.h>
static uint32_t levels[9];
static int sensor_fps,read_error,set_error,sensor_error,wrong_readback,underwrite,deferred_reads,pending_fps,never_complete;
static unsigned level_sets,sensor_sets;
static int get_levels(int dev,void *out)
{assert(!dev);if(read_error)return read_error;memcpy(out,levels,sizeof levels);if(underwrite)((unsigned char*)out)[-1]=0;return 0;}
static int set_levels(int dev,void *in)
{assert(!dev);level_sets++;if(set_error)return set_error;memcpy(levels,in,sizeof levels);if(wrong_readback)levels[2]++;return 0;}
static int get_sensor(int dev){assert(!dev);if(pending_fps&&!never_complete&&!--deferred_reads){sensor_fps=pending_fps;pending_fps=0;}return sensor_fps;}
static int set_sensor(int dev,void *in)
{assert(!dev);sensor_sets++;if(sensor_error)return sensor_error;if(deferred_reads)pending_fps=*(int*)in;else if(!wrong_readback)sensor_fps=*(int*)in;return 0;}
static void reset(void)
{uint32_t initial[9]={25,1874,16,0,0,0,14,2250,8};memcpy(levels,initial,sizeof levels);sensor_fps=14;read_error=set_error=sensor_error=wrong_readback=underwrite=deferred_reads=pending_fps=never_complete=0;level_sets=sensor_sets=0;}
int main(void)
{
 struct anyka_sensor_timing t={get_sensor,set_sensor,get_levels,set_levels};
 reset();assert(!anyka_sensor_timing_apply(&t,25));
 assert(sensor_fps==25&&level_sets==1&&sensor_sets==1&&levels[0]==25&&levels[1]==1124&&levels[6]==25&&levels[7]==1124&&levels[2]==16&&levels[8]==8&&!levels[3]&&!levels[4]&&!levels[5]);
 assert(!anyka_sensor_timing_apply(&t,25)&&level_sets==1&&sensor_sets==2); /* force equal sensor request */
 reset();levels[3]=20;levels[4]=1000;levels[5]=12;assert(!anyka_sensor_timing_apply(&t,15));assert(levels[0]==15&&levels[1]==1874&&levels[3]==15&&levels[4]==1000&&levels[5]==12&&levels[6]==15&&levels[7]==1874);
 reset();read_error=7;assert(anyka_sensor_timing_apply(&t,25)==-EIO&&!level_sets&&!sensor_sets);
 reset();underwrite=1;assert(anyka_sensor_timing_apply(&t,25)==-EOVERFLOW&&!level_sets&&!sensor_sets);
 reset();levels[0]=0;assert(anyka_sensor_timing_apply(&t,25)==-EPROTO&&!level_sets&&!sensor_sets);
 reset();set_error=-EIO;assert(anyka_sensor_timing_apply(&t,25)==-EIO&&level_sets==1&&!sensor_sets);
 reset();wrong_readback=1;assert(anyka_sensor_timing_apply(&t,25)==-EIO&&level_sets==1&&!sensor_sets);
 reset();sensor_error=3;assert(anyka_sensor_timing_apply(&t,25)==-EIO&&sensor_sets==1);
 reset();deferred_reads=4;assert(!anyka_sensor_timing_apply(&t,25)&&sensor_fps==25&&sensor_sets==1);
 reset();deferred_reads=4;never_complete=1;assert(anyka_sensor_timing_apply(&t,25)==-EIO&&sensor_fps==14&&sensor_sets==1);
 reset();sensor_fps=0;assert(anyka_sensor_timing_apply(&t,25)==-EPROTO&&!sensor_sets);
 reset();assert(anyka_sensor_timing_apply(&t,30)==-EINVAL&&!sensor_sets&&!level_sets);
 struct anyka_sensor_timing missing={0};assert(anyka_sensor_timing_apply(&missing,25)==-EINVAL);
 assert(anyka_sensor_timing_bind(NULL)==-EINVAL&&anyka_sensor_timing_bind(&missing)==-ENOSYS);
 puts("PASS sensor timing: exact scalar getter, guarded9-word levels, fixed active rates, exposure caps, preserved inactive middle/gain policy, explicit request/readback, fail closed");
}
