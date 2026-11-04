#define _GNU_SOURCE
#include "../../src/anyka/video_mode.c"
#include <assert.h>
static void malformed(int dir,unsigned index,unsigned value)
{
 int fd=openat(dir,"video-mode.conf",O_RDWR|O_NOFOLLOW);assert(fd>=0);
 unsigned char byte=(unsigned char)value;assert(pwrite(fd,&byte,1,index)==1);close(fd);
 struct anyka_video_mode before={1,2,3,4},out=before;
 assert(anyka_video_mode_load(dir,&out)<0&&!memcmp(&before,&out,sizeof out));
}
int main(int argc,char **argv)
{
 assert(argc==2);int dir=open(argv[1],O_RDONLY|O_DIRECTORY);assert(dir>=0);
 struct anyka_video_mode mode={0},out={0};assert(!anyka_video_mode_load(dir,&mode));
 assert(mode.width==1920&&mode.height==1080&&mode.fps==15&&mode.profile==0);
 for(unsigned geometry=0;geometry<2;geometry++)for(unsigned rate=0;rate<2;rate++)for(unsigned profile=0;profile<2;profile++){
  mode=(struct anyka_video_mode){geometry?1280:1920,geometry?720:1080,rate?25:15,profile?2:0};
  assert(!anyka_video_mode_validate(&mode)&&!anyka_video_mode_save(dir,&mode)&&!anyka_video_mode_load(dir,&out));
  assert(!memcmp(&mode,&out,sizeof mode));
 }
 struct anyka_video_mode invalids[]={{1920,1080,30,0},{1920,1080,60,0},{1920,1080,0,0},
  {1920,720,15,0},{1280,1080,15,0},{1920,1080,15,1},{1920+65536u,1080,15,0},{0,0,15,0}};
 for(unsigned i=0;i<sizeof invalids/sizeof invalids[0];i++)assert(anyka_video_mode_validate(&invalids[i])==-EINVAL&&anyka_video_mode_save(dir,&invalids[i])==-EINVAL);
 malformed(dir,0,'X');assert(!anyka_video_mode_save(dir,&mode));
 malformed(dir,6,'2');assert(!anyka_video_mode_save(dir,&mode));
 malformed(dir,14,1);assert(!anyka_video_mode_save(dir,&mode));
 malformed(dir,12,30);assert(!anyka_video_mode_save(dir,&mode));
 int fd=openat(dir,"video-mode.conf",O_RDWR);assert(fd>=0);assert(!fchmod(fd,0644));close(fd);
 assert(anyka_video_mode_load(dir,&out)<0&&anyka_video_mode_save(dir,&mode)<0);
 assert(!fchmodat(dir,"video-mode.conf",0600,0));
 assert(!linkat(dir,"video-mode.conf",dir,"shared",0));assert(anyka_video_mode_load(dir,&out)<0&&anyka_video_mode_save(dir,&mode)<0);
 assert(!unlinkat(dir,"shared",0));
 assert(!renameat(dir,"video-mode.conf",dir,"other"));assert(!symlinkat("other",dir,"video-mode.conf"));
 assert(anyka_video_mode_load(dir,&out)<0&&anyka_video_mode_save(dir,&mode)<0);assert(!unlinkat(dir,"video-mode.conf",0));
 assert(!anyka_video_mode_save(dir,&mode));assert(!fchmod(dir,0755));assert(anyka_video_mode_load(dir,&out)<0);
 assert(!fchmod(dir,0700));close(dir);
 puts("Anyka startup modes: eight supported choices, defaults, strict16-byte persistence, corruption/permissions/link refusal PASS");return 0;
}
