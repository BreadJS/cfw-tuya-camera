/* One-shot borrowed ISP control-plane trial; never opens VMM or stops media.
 * Exact driver open/release refcount and gamma wire contract are recorded in
 * docs/GAMMA.md. Requires the retained production owner; not a fresh owner. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "day_gamma.h"
#include "day_tone_trial.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
static const char backup_path[]="/tmp/camera-gamma-backup.bin";
struct backup {char magic[8];uint64_t ticks;uint32_t route;uint32_t gamma[163];};
static uint64_t pinned_ticks;static unsigned long owner_pid;
static int owner(void){
 char path[128],buf[4096],exe[256];snprintf(path,sizeof path,"/proc/%lu/exe",owner_pid);ssize_t n=readlink(path,exe,sizeof exe-1);if(n<=0)return -1;exe[n]=0;if(strcmp(exe,"/usr/sbin/camera-service"))return -1;
 snprintf(path,sizeof path,"/proc/%lu/stat",owner_pid);FILE *f=fopen(path,"r");if(!f)return -1;char *ok=fgets(buf,sizeof buf,f);fclose(f);if(!ok)return -1;char *p=strrchr(buf,')');if(!p)return -1;char *save=NULL,*tok=strtok_r(p+2," ",&save);for(unsigned i=3;i<22&&tok;i++)tok=strtok_r(NULL," ",&save);if(!tok||strtoull(tok,NULL,10)!=pinned_ticks)return -1;
 struct stat dev,st;if(stat("/dev/isp",&dev)||!S_ISCHR(dev.st_mode))return -1;
 snprintf(path,sizeof path,"/proc/%lu/fd",owner_pid);DIR *d=opendir(path);if(!d)return -1;int found=0;struct dirent *e;
 while((e=readdir(d))){if(e->d_name[0]=='.')continue;char name[512];snprintf(name,sizeof name,"%s/%s",path,e->d_name);if(!stat(name,&st)&&S_ISCHR(st.st_mode)&&st.st_rdev==dev.st_rdev)found=1;}
 closedir(d);return found?0:-1;
}
static int store(const struct backup *b){int fd=open(backup_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;ssize_t n=write(fd,b,sizeof *b);int rc=n==sizeof *b&&!fsync(fd)?0:-1;if(close(fd))rc=-1;return rc;}
static int load(struct backup *b){int fd=open(backup_path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;struct stat st;int rc=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&077)||st.st_nlink!=1||st.st_size!=sizeof *b;ssize_t n=rc?-1:read(fd,b,sizeof *b);close(fd);return rc||n!=sizeof *b||memcmp(b->magic,"GAMMA01",8)||b->ticks!=pinned_ticks?-1:0;}
static int tone_trial(volatile uint32_t *reg,int restore){
 struct tone_backup {char magic[8];uint64_t ticks;uint32_t control,values[sizeof(tone_words)/sizeof(tone_words[0])];} b={.magic="TONE001",.ticks=pinned_ticks};
 const char *path="/tmp/camera-tone-backup.bin";int fd=open(path,(restore?O_RDONLY:O_WRONLY|O_CREAT|O_EXCL)|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 9;
 if(restore){struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||(st.st_mode&077)||st.st_size!=sizeof b){close(fd);return 9;}ssize_t n=read(fd,&b,sizeof b);close(fd);if(n!=sizeof b||memcmp(b.magic,"TONE001",8)||b.ticks!=pinned_ticks)return 9;}
 else {b.control=reg[0x24/4];for(unsigned i=0;i<sizeof(tone_words)/sizeof(tone_words[0]);i++)b.values[i]=reg[tone_words[i].offset/4];ssize_t n=write(fd,&b,sizeof b);int rc=fsync(fd);close(fd);if(n!=sizeof b||rc)return 9;}
 if(owner())return 3;
 reg[0x24/4]|=0x40000;__sync_synchronize();
 for(unsigned i=0;i<sizeof(tone_words)/sizeof(tone_words[0]);i++){const struct tone_word *w=&tone_words[i];reg[w->offset/4]=(reg[w->offset/4]&~w->mask)|((restore?b.values[i]:w->value)&w->mask);__sync_synchronize();}
 reg[0x24/4]=(reg[0x24/4]&~UINT32_C(0x40000))|(restore?(b.control&0x40000):0);__sync_synchronize();
 printf("TONE_TRIAL restored=%d reg24=%08x profile_gain_q6=814 adaptive_controller=no\n",restore,reg[0x24/4]);if(restore){unlink(path);}
 return owner()?3:0;
}
static const struct detail_word {uint32_t off,mask,value;} detail_words[]={
{0x4f4u,0x7u,0x0u},
{0x4f8u,0xffffff00u,0xf0000u},
{0x4fcu,0xffffffffu,0x13000fu},
{0x514u,0xffffffffu,0x590u},
{0x518u,0xffffffffu,0x5001c000u},
{0x51cu,0xffffffffu,0x8001c000u},
{0xa0cu,0xffffffffu,0xe000eu},
{0xa10u,0xffffffffu,0x90009u},
{0xa14u,0xffffffffu,0x40004u},
{0xa18u,0xffffffffu,0x40004u},
{0xa1cu,0xffffffffu,0x20004u},
{0xa20u,0xffffffffu,0x20002u},
{0xa24u,0xffffffffu,0x10001u},
{0xa28u,0xffffffffu,0x10001u},
{0xa2cu,0xffffffffu,0x0u},
{0xa30u,0xffffffffu,0x0u},
{0xa34u,0xffffffffu,0x0u},
{0xa38u,0xffffffffu,0x0u},
{0xa3cu,0xffffffffu,0x0u},
{0xa40u,0xffffffffu,0x0u},
{0xa44u,0xffffffffu,0x0u},
{0xa48u,0xffffffffu,0x0u},
{0xa4cu,0xffffffffu,0x0u},
{0xa50u,0xffffffffu,0x0u},
{0xa54u,0xffffffffu,0x0u},
{0xa58u,0xffffffffu,0x0u},
{0xa5cu,0xffffffffu,0x0u},
{0xa60u,0xffffffffu,0x0u},
{0xa64u,0xffffffffu,0x0u},
{0xa68u,0xffffffffu,0x0u},
};
static int detail_trial(volatile uint32_t *reg,int restore){
 struct detail_backup {char magic[8];uint64_t ticks;uint32_t values[sizeof(detail_words)/sizeof(detail_words[0])];} b={.magic="DETAIL1",.ticks=pinned_ticks};
 const char *path="/tmp/camera-detail-backup.bin";int fd=open(path,(restore?O_RDONLY:O_WRONLY|O_CREAT|O_EXCL)|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return 9;
 if(restore){struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||(st.st_mode&077)||st.st_size!=sizeof b){close(fd);return 9;}ssize_t n=read(fd,&b,sizeof b);close(fd);if(n!=sizeof b||memcmp(b.magic,"DETAIL1",8)||b.ticks!=pinned_ticks)return 9;}
 else {for(unsigned i=0;i<sizeof(detail_words)/sizeof(detail_words[0]);i++)b.values[i]=reg[detail_words[i].off/4];ssize_t n=write(fd,&b,sizeof b);int rc=fsync(fd);close(fd);if(n!=sizeof b||rc)return 9;}
 if(owner())return 3;
 for(unsigned i=0;i<sizeof(detail_words)/sizeof(detail_words[0]);i++){const struct detail_word *w=&detail_words[i];uint32_t before=reg[w->off/4];reg[w->off/4]=(before&~w->mask)|((restore?b.values[i]:w->value)&w->mask);__sync_synchronize();printf("DETAIL_REG offset=%03x before=%08x after=%08x\n",w->off,before,reg[w->off/4]);}
 if(restore){unlink(path);}
 return owner()?3:0;
}
int main(int argc,char **argv){
 if(argc!=4||geteuid()!=0)return 2;
 owner_pid=strtoul(argv[1],NULL,10);pinned_ticks=strtoull(argv[2],NULL,10);
 if(owner_pid<2||!pinned_ticks||owner())return 3;
 int detail=!strcmp(argv[3],"detail-profile"),undetail=!strcmp(argv[3],"restore-detail");
 int moderate=!strcmp(argv[3],"color-moderate");
 int tone=!strcmp(argv[3],"stock-tone"),untone=!strcmp(argv[3],"restore-tone");
 int awb_window=!strcmp(argv[3],"awb-window");
 int drc_off=!strcmp(argv[3],"drc-bypass"),drc_on=!strcmp(argv[3],"drc-enable");
 int inspect=!strcmp(argv[3],"inspect"),restore=!strcmp(argv[3],"restore");if(!detail&&!undetail&&!moderate&&!tone&&!untone&&!awb_window&&!drc_off&&!drc_on&&!inspect&&!restore&&strcmp(argv[3],"stock-day"))return 2;
 int mem=open("/dev/mem",(inspect?O_RDONLY:O_RDWR)|O_CLOEXEC);if(mem<0)return 4;
 volatile uint32_t *reg=mmap(NULL,0x4000,PROT_READ|(inspect?0:PROT_WRITE),MAP_SHARED,mem,(off_t)UINT32_C(0xe8400000));close(mem);if(reg==MAP_FAILED)return 4;
 if(detail||undetail){int rc=detail_trial(reg,undetail);munmap((void *)reg,0x4000);return rc;}
 if(moderate){if(owner())return 3;uint32_t before=reg[0x5d0/4];if(!(before&0x3ff))return 10;reg[0x5d0/4]=(before&~UINT32_C(0x3ff))|320;__sync_synchronize();printf("COLOR_TRIAL before=%08x after=%08x\n",before,reg[0x5d0/4]);munmap((void *)reg,0x4000);return owner()?3:0;}
 if(tone||untone){int rc=tone_trial(reg,untone);munmap((void *)reg,0x4000);return rc;}
 if(awb_window){if(owner())return 3;const uint32_t offsets[]={0x18c,0x190,0x194,0x198,0x19c,0x1a0};const uint32_t values[]={0x00800830,0x12f7992f,0x2405d240,0x230565e2,0x1ad8c159,0x000ffbfe};for(unsigned i=0;i<6;i++){printf("AWB_WINDOW offset=%03x before=%08x after=%08x\n",offsets[i],reg[offsets[i]/4],values[i]);reg[offsets[i]/4]=values[i];__sync_synchronize();}munmap((void *)reg,0x4000);return owner()?3:0;}
 if(drc_off||drc_on){if(owner())return 3;uint32_t before=reg[0x24/4];reg[0x24/4]=(before&~UINT32_C(0x40000))|(drc_off?UINT32_C(0x40000):0);__sync_synchronize();printf("DRC_TRIAL before=%08x after=%08x\n",before,reg[0x24/4]);munmap((void *)reg,0x4000);return owner()?3:0;}
 printf("GAMMA_BEFORE reg28=%08x reg4f0=%08x RGB0=%08x Y0=%08x\n",reg[0x28/4],reg[0x4f0/4],reg[0x1000/4],reg[0x1480/4]);
 if(inspect){printf("ISP_REG offset=024 value=%08x\n",reg[0x24/4]);for(unsigned off=0x18c;off<=0x1a0;off+=4)printf("ISP_REG offset=%03x value=%08x\n",off,reg[off/4]);for(unsigned off=0x4d8;off<=0x4f0;off+=4)printf("ISP_REG offset=%03x value=%08x\n",off,reg[off/4]);for(unsigned off=0x5c8;off<=0x5d0;off+=4)printf("ISP_REG offset=%03x value=%08x\n",off,reg[off/4]);munmap((void *)reg,0x4000);return owner()?3:0;}
 struct backup b={.magic="GAMMA01",.ticks=pinned_ticks,.route=reg[0x4f0/4]};const uint32_t *packet=camera_day_gamma;
 if(restore){if(load(&b))return 5;packet=b.gamma;}
 else{
  for(unsigned i=0;i<80;i++){b.gamma[i]=reg[(0x1480+4*i)/4];b.gamma[80+i]=reg[(0x1000+4*i)/4];if(b.gamma[80+i]!=reg[(0x1180+4*i)/4]||b.gamma[80+i]!=reg[(0x1300+4*i)/4])return 6;}
  b.gamma[160]=(reg[0x28/4]&0x400)?0:1;b.gamma[161]=(reg[0x28/4]&0x10)?0:1;b.gamma[162]=1;if(store(&b))return 5;
 }
 if(owner())return 3;
 int fd=open("/dev/isp",O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 7;if(owner()){close(fd);return 3;}
 uint32_t wire[163];memcpy(wire,packet,sizeof wire);int rc=ioctl(fd,(unsigned long)CAMERA_GAMMA_SET,wire);int error=errno;
 if(!rc){__sync_synchronize();reg[0x4f0/4]=(reg[0x4f0/4]&~UINT32_C(0x10))|(restore?(b.route&0x10):0);__sync_synchronize();usleep(300000);}
 printf("GAMMA_AFTER ioctl=%d errno=%d reg28=%08x reg4f0=%08x RGB0=%08x Y0=%08x owner=%s\n",rc,rc?-error:0,reg[0x28/4],reg[0x4f0/4],reg[0x1000/4],reg[0x1480/4],owner()?"lost":"retained");
 close(fd);munmap((void *)reg,0x4000);if(!rc&&restore)unlink(backup_path);return rc?8:0;
}
