#define _GNU_SOURCE
#include "ramroot_expected.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>
static size_t position;
static inline int source_readonly(void)
{
 struct statvfs flags;
 if(statvfs("/",&flags))return -1;
 if(flags.f_flag&ST_RDONLY)return 0;
#ifdef CAMERA_NOR_RAM_ROOT
 /* The factory ARM ATAG_CORE clears MS_RDONLY unless bootargs contains ro.
  * SquashFS is intrinsically read-only, but statvfs reports the separate mount
  * flag. Normalize that flag before extracting; never remount FAT/config. */
 struct statfs fs;
 if(statfs("/",&fs))return -1;
 if((unsigned long)fs.f_type!=0x73717368UL){errno=EINVAL;return -1;}
 if(mount(NULL,"/",NULL,MS_REMOUNT|MS_RDONLY,NULL))return -1;
 if(statvfs("/",&flags))return -1;
 if(flags.f_flag&ST_RDONLY)return 0;
#endif
 errno=EROFS;return -1;
}
static int exact(int fd,void *out,size_t bytes)
{unsigned char *p=out;if(bytes>RAMROOT_BYTES-position)return -1;while(bytes){ssize_t n=read(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;bytes-=(size_t)n;position+=(size_t)n;}return 0;}
static int write_all(int fd,const void *in,size_t size)
{const unsigned char*p=in;while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}return 0;}
static int verify(int fd)
{
 struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=RAMROOT_BYTES)return -1;
 crypto_blake2b_ctx ctx;crypto_blake2b_init(&ctx,64);unsigned char block[16384],digest[64];size_t left=RAMROOT_BYTES;
 while(left){size_t want=left<sizeof block?left:sizeof block;ssize_t n=read(fd,block,want);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;crypto_blake2b_update(&ctx,block,(size_t)n);left-=(size_t)n;}
 crypto_blake2b_final(&ctx,digest);if(crypto_verify64(digest,ramroot_hash))return -1;
 return lseek(fd,0,SEEK_SET)==0?0:-1;
}
static int hex(const char *p,uint32_t *out)
{uint32_t value=0;for(unsigned i=0;i<8;i++){unsigned char c=(unsigned char)p[i];unsigned n;if(c>='0'&&c<='9')n=c-'0';else if(c>='a'&&c<='f')n=c-'a'+10;else if(c>='A'&&c<='F')n=c-'A'+10;else return -1;value=(value<<4)|n;}*out=value;return 0;}
static int safe_name(const char *name)
{
 if(!*name||*name=='/'||strlen(name)>511)return -1;
 for(const char *p=name;*p;){const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);if(!n||(n==1&&*p=='.')||(n==2&&p[0]=='.'&&p[1]=='.'))return -1;if(!end)break;p=end+1;}
 return 0;
}
static int parent(int root,const char *name,char leaf[512])
{
 char path[512];strcpy(path,name);char *last=strrchr(path,'/');int fd=dup(root);if(fd<0)return -1;
 if(!last){strcpy(leaf,path);return fd;}*last=0;strcpy(leaf,last+1);
 for(char *p=path;*p;){char *end=strchr(p,'/');if(end)*end=0;int next=openat(fd,p,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);close(fd);if(next<0)return -1;fd=next;if(!end)break;p=end+1;}
 return fd;
}
static int align4(int fd)
{unsigned char pad[3];size_t n=(4-(position&3))&3;if(exact(fd,pad,n))return -1;for(size_t i=0;i<n;i++)if(pad[i])return -1;return 0;}
static int extract(int fd,int root)
{
 position=0;unsigned count=0;umask(0);
 for(;;){char header[110],name[512];uint32_t field[13];
  if(exact(fd,header,sizeof header)||memcmp(header,"070701",6))return -1;
  for(unsigned i=0;i<13;i++)if(hex(header+6+i*8,&field[i]))return -1;
  uint32_t mode=field[1],size=field[6],names=field[11];
  if(field[2]||field[3]||field[12]||names<1||names>sizeof name||size>RAMROOT_BYTES||exact(fd,name,names)||name[names-1]||strlen(name)!=names-1||align4(fd))return -1;
  if(!strcmp(name,"TRAILER!!!")){if(size)return -1;break;}
  if(++count>4096)return -1;
  if(!strcmp(name,".")){if((mode&S_IFMT)!=S_IFDIR||size||fchmod(root,mode&0777))return -1;continue;}
  if(safe_name(name))return -1;
  char leaf[512];int dir=parent(root,name,leaf);if(dir<0)return -1;int rc=0;
  if((mode&S_IFMT)==S_IFDIR){if(size||mkdirat(dir,leaf,mode&0777))rc=-1;}
  else if((mode&S_IFMT)==S_IFREG){
   if(field[4]!=1)rc=-1;
   int file=rc?-1:openat(dir,leaf,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,mode&0777);if(file<0)rc=-1;
   unsigned char block[16384];size_t left=size;
   while(!rc&&left){size_t want=left<sizeof block?left:sizeof block;if(exact(fd,block,want)||write_all(file,block,want))rc=-1;else left-=want;}
   if(file>=0&&close(file))rc=-1;
  }else if((mode&S_IFMT)==S_IFLNK){char target[1024];if(!size||size>=sizeof target||exact(fd,target,size))rc=-1;else{target[size]=0;if(memchr(target,0,size)||symlinkat(target,dir,leaf))rc=-1;}}
  else if((mode&S_IFMT)==S_IFCHR){
   if(size||!((!strcmp(name,"dev/console")&&field[9]==5&&field[10]==1)||(!strcmp(name,"dev/null")&&field[9]==1&&field[10]==3)))rc=-1;
#ifndef RAMINIT_HOST_TEST
   else if(mknodat(dir,leaf,mode,makedev(field[9],field[10])))rc=-1;
#endif
  }else rc=-1;
  close(dir);if(rc||align4(fd))return -1;
 }
 unsigned char block[512];while(position<RAMROOT_BYTES){size_t n=RAMROOT_BYTES-position;if(n>sizeof block)n=sizeof block;if(exact(fd,block,n))return -1;for(size_t i=0;i<n;i++)if(block[i])return -1;}
 printf("RAMINIT_EXTRACT_PASS entries=%u bytes=%zu\n",count,position);return 0;
}
#ifndef RAMINIT_HOST_TEST
static void fail(const char *phase)
{fprintf(stderr,"RAMINIT_FAIL phase=%s errno=%d no_NOR_writes=yes UART_recovery_required=yes\n",phase,errno);fflush(NULL);for(;;)pause();}
#ifndef CAMERA_NOR_RAM_ROOT
static int trial_file(const char *path,const char *target)
{
 int source=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(source<0)return errno==ENOENT?0:-1;
 struct stat st;if(fstat(source,&st)||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>16384){close(source);return -1;}
 int destination=open(target,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(destination<0){close(source);return -1;}
 unsigned char block[4096];size_t done=0;int rc=0;
 while(done<(size_t)st.st_size){size_t want=(size_t)st.st_size-done;if(want>sizeof block)want=sizeof block;ssize_t n=read(source,block,want);if(n<0&&errno==EINTR)continue;if(n<=0||write_all(destination,block,(size_t)n)){rc=-1;break;}done+=(size_t)n;}
 close(source);if(close(destination))rc=-1;memset(block,0,sizeof block);
 if(!rc)printf("RAMINIT_TRIAL_CONFIG bytes=%zu source=SD destination=RAM\n",done);
 return rc;
}
#endif
#endif
int main(int argc,char **argv)
{
 if(argc==3&&!strcmp(argv[1],"--verify-archive")){int fd=open(argv[2],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);int rc=fd<0?-1:verify(fd);if(fd>=0)close(fd);puts(rc?"RAMINIT_VERIFY_FAIL":"RAMINIT_VERIFY_PASS");return rc?1:0;}
#ifdef RAMINIT_HOST_TEST
 if(argc==4&&!strcmp(argv[1],"--extract-test")){int fd=open(argv[2],O_RDONLY|O_CLOEXEC|O_NOFOLLOW),root=open(argv[3],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0||root<0||verify(fd))return 1;int rc=extract(fd,root);close(fd);close(root);return rc?1:0;}
 return 2;
#else
 struct utsname u;struct statfs fs;struct statvfs vfs;struct stat source_root;
 const unsigned long source_magic=
#ifdef CAMERA_NOR_RAM_ROOT
 0x73717368UL; /* SquashFS, read-only firmware container. */
#else
 0x4d44UL; /* Explicit FAT/SD trial. */
#endif
 setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
 /* A failed PID1 precondition must identify itself, then remain available for
  * UART recovery. Exiting silently kills init and hides the failing operand. */
 if(getpid()!=1)return 1;
 printf("RAMROOT_PREFLIGHT argc=%d uid=%lu gid=%lu\n",argc,(unsigned long)geteuid(),(unsigned long)getegid());
 if(argc!=1){errno=EINVAL;fail("init-arguments");}
 if(geteuid()!=0||getegid()!=0){errno=EPERM;fail("init-identity");}
 if(uname(&u))fail("uname");
 printf("RAMROOT_KERNEL release=%s version=%s\n",u.release,u.version);
 if(strcmp(u.release,"4.9.129")||strcmp(u.version,"#55 PREEMPT Sat Dec 11 11:44:24 CST 2021")){errno=EINVAL;fail("kernel-identity");}
 if(statfs("/",&fs))fail("source-statfs");
 printf("RAMROOT_SOURCE type=0x%lx expected=0x%lx\n",(unsigned long)fs.f_type,source_magic);
 if((unsigned long)fs.f_type!=source_magic){errno=EINVAL;fail("source-filesystem");}
 if(statvfs("/",&vfs))fail("source-statvfs");
 printf("RAMROOT_SOURCE flags=0x%lx\n",(unsigned long)vfs.f_flag);
 if(source_readonly())fail("source-require-readonly");
 if(stat("/",&source_root))fail("source-stat");
#ifdef CAMERA_NOR_RAM_ROOT
 puts("RAMROOT_START root=readonly_SquashFS destination=RAM no_NOR_writes=yes");
#else
 puts("RAMINIT_START root=readonly_FAT destination=RAM no_NOR_mount_or_write=yes");
#endif
 int fd=open("/ROOTFS.CPI",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0||verify(fd))fail("archive-integrity");
 if(mount("tmpfs","/RAMROOT","tmpfs",0,"mode=755"))fail("mount-tmpfs");
 int root=open("/RAMROOT",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(root<0||extract(fd,root))fail("extract");close(fd);
 /* A root-owned runtime witness distinguishes a real installed-NOR boot
  * from an otherwise healthy SD/recovery trial. Not part of the archive and
  * never accepted from the browser or a self-reported release string. */
 const char *origin="RECOVERY_ROOT_V1\n";
#ifdef CAMERA_NOR_RAM_ROOT
 if(major(source_root.st_dev)==31&&minor(source_root.st_dev)==4)origin="NOR_ROOT_V1\n";
#endif
 int witness=openat(root,".camera-boot-origin",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0400);
 if(witness<0||write_all(witness,origin,strlen(origin))||close(witness))fail("boot-origin-witness");
 close(root);
#ifndef CAMERA_NOR_RAM_ROOT
 if(trial_file("/TRIALWPA.CFG","/RAMROOT/usr/share/camera/trial-wifi.conf")||
    trial_file("/TRIALSET.CFG","/RAMROOT/usr/share/camera/trial-settings.conf")||
    trial_file("/TRIALREC.CFG","/RAMROOT/usr/share/camera/trial-recovery.conf")||
    trial_file("/TRIALAUTH.CFG","/RAMROOT/usr/share/camera/trial-credentials.conf"))fail("trial-config-copy");
#endif
 if(mkdir("/RAMROOT/.old-root",0700)||syscall(SYS_pivot_root,"/RAMROOT","/RAMROOT/.old-root")||chdir("/"))fail("pivot-root");
 if(umount2("/.old-root",MNT_DETACH)||rmdir("/.old-root"))fail("detach-FAT");
#ifdef CAMERA_NOR_RAM_ROOT
 puts("RAMROOT_HANDOFF root=RAM old_firmware_container=detached init=/sbin/init");
#else
 puts("RAMINIT_HANDOFF root=RAM config=RAM old_SD_root=detached init=/sbin/init");
#endif
 char *args[]={"/sbin/init",NULL};execv(args[0],args);fail("exec-init");return 1;
#endif
}
