#define _GNU_SOURCE
#include "../../src/rotation/settings.h"
#include "../../src/native/sensor/mis2006_runtime.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
struct fake {unsigned writes,reads,fail_write,fail_read,mismatch;uint8_t reg[65536];};
static int write_reg(void *p,const uint8_t wire[3]){struct fake *f=p;unsigned address=(unsigned)wire[0]*256+wire[1];assert(address==0x3007||address==0x3205||address==0x3207||address==0x3209||address==0x320b);if(++f->writes==f->fail_write)return -EIO;f->reg[address]=wire[2];return 0;}
static int read_reg(void *p,const uint8_t wire[2],uint8_t *out){struct fake *f=p;unsigned address=(unsigned)wire[0]*256+wire[1];if(++f->reads==f->fail_read)return -EIO;*out=f->reg[address]^(f->reads==f->mismatch);return 0;}
static struct mis2006_runtime sensor(struct fake *f){struct mis2006_runtime s={.state=MIS2006_ACTIVE,.opaque=f};s.io.write=write_reg;s.io.read=read_reg;return s;}
static void hardware_contract(void)
{
 struct fake f={0};struct mis2006_runtime s=sensor(&f);
 assert(mis2006_set_rotation(&s,2)==-EINVAL&&!f.writes);
 assert(!mis2006_set_rotation(&s,1)&&f.writes==5&&f.reads==5);
 assert(f.reg[0x3007]==0&&f.reg[0x3205]==4&&f.reg[0x3207]==0x43&&f.reg[0x3209]==4&&f.reg[0x320b]==0x8b);
 assert(!mis2006_set_rotation(&s,0)&&f.writes==10&&f.reads==10);
 assert(f.reg[0x3007]==3&&f.reg[0x3205]==5&&f.reg[0x3207]==0x44&&f.reg[0x3209]==5&&f.reg[0x320b]==0x8c);
 for(unsigned mode=0;mode<3;mode++)for(unsigned i=1;i<=5;i++){
  memset(&f,0,sizeof f);s=sensor(&f);if(mode==0)f.fail_write=i;else if(mode==1)f.fail_read=i;else f.mismatch=i;
  assert(mis2006_set_rotation(&s,1)==(mode==2?-EPROTO:-EIO)&&s.state==MIS2006_FAULT);
  unsigned n=f.writes;assert(mis2006_set_rotation(&s,0)==-EPIPE&&f.writes==n);
 }
}
int main(int argc,char **argv)
{
 assert(argc==2);int dir=open(argv[1],O_DIRECTORY|O_RDONLY);assert(dir>=0);unsigned enabled=99;
 assert(!camera_rotation_load(dir,&enabled)&&enabled==0);
 assert(!camera_rotation_save(dir,1));enabled=0;assert(!camera_rotation_load(dir,&enabled)&&enabled==1);
 struct stat st;assert(!fstatat(dir,"rotation.conf",&st,0)&&(st.st_mode&07777)==0600);
 assert(camera_rotation_save(dir,2)==-1);assert(!camera_rotation_load(dir,&enabled)&&enabled==1);
 assert(!camera_rotation_save(dir,0));enabled=1;assert(!camera_rotation_load(dir,&enabled)&&enabled==0);
 int fd=openat(dir,"rotation.conf",O_WRONLY);assert(fd>=0);assert(pwrite(fd,"\2",1,8)==1);close(fd);enabled=77;
 assert(camera_rotation_load(dir,&enabled)==-1&&enabled==77);
 assert(!unlinkat(dir,"rotation.conf",0));assert(!symlinkat("outside",dir,"rotation.conf"));
 assert(camera_rotation_load(dir,&enabled)==-1&&camera_rotation_save(dir,0)==-1);
 assert(!unlinkat(dir,"rotation.conf",0));assert(!camera_rotation_save(dir,1));assert(!fchmodat(dir,"rotation.conf",0644,0));
 assert(camera_rotation_load(dir,&enabled)==-1&&camera_rotation_save(dir,0)==-1);
 close(dir);hardware_contract();puts("Rotation: persistence/defaults/private-file checks, exact0/180 crop phases,15 I2C/readback failures PASS");return 0;
}
