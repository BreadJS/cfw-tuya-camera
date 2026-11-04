#include "../../src/native/isp/isp_shadow_runtime.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static uint32_t reg[0x4000/4];static unsigned reads,writes,fail_at;
int fh_linux_io_isp_read(void *io,uint32_t off,uint32_t *out){(void)io;assert(off<sizeof reg&&!(off&3));reads++;*out=reg[off/4];return 0;}
int fh_linux_io_isp_write(void *io,uint32_t off,uint32_t value){(void)io;assert(off<sizeof reg&&!(off&3));writes++;if(writes==fail_at)return -EIO;reg[off/4]=value;return 0;}
static void reset(void){memset(reg,0,sizeof reg);reg[0x24/4]=0x0ae5eb0a;reg[0x23c/4]=0x34620;reg[0x24c/4]=512;reads=writes=0;}
int main(void)
{
 struct fh_linux_io io={0};struct fh_shadow_runtime runtime={0};
 struct fh_isp_stats_sample sample={.generation=1,.shadow_valid=1,.shadow_width=1927,.shadow_height=1087,
  .shadow_histogram={383045508,886088540,58976},.shadow_min_log_q8=2464,.shadow_max_log_q8=3954};
 reset();assert(fh_shadow_update(&io,&runtime,&sample,814)==1&&runtime.state.generation==1&&writes==50);
 assert(reg[0x24/4]==0x0ae1eb0a&&!(reg[0x23c/4]&255)); /* Static low byte survives adaptive publication. */
 unsigned completed=writes;assert(fh_shadow_update(&io,&runtime,&sample,814)==-ESTALE&&writes==completed);
 for(fail_at=1;fail_at<=50;fail_at++){
  reset();memset(&runtime,0,sizeof runtime);assert(fh_shadow_update(&io,&runtime,&sample,814)==-EIO&&runtime.fault==-EIO&&!runtime.state.generation&&writes==fail_at);
  completed=writes;assert(fh_shadow_update(&io,&runtime,&sample,814)==-EPIPE&&writes==completed);
 }
 fail_at=0;reset();memset(&runtime,0,sizeof runtime);sample.shadow_valid=0;
 assert(!fh_shadow_update(&io,&runtime,&sample,814)&&!writes&&!reads&&!runtime.state.generation);
 sample.shadow_valid=1;sample.shadow_histogram[2]=70000;
 assert(fh_shadow_update(&io,&runtime,&sample,814)==-EINVAL&&!writes&&!runtime.state.generation);
 return 0;
}
