#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "capsule.h"
#include "receipt.h"
#include "vendor/monocypher.h"
#include <update_trust.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>
#define STATE "/etc/config/camera/update"
static const uint32_t sizes[8]={0x32000,0x1000,0x1000,0xc000,0x180000,0x100000,0x40000,ANYKA_APP_BYTES};
static const uint32_t offsets[8]={0,0x32000,0x33000,0x34000,0x40000,0x1c0000,0x2c0000,0x300000};
static const char *names[8]={"UBOOT","ENV","ENVBK","DTB","KERNEL","ROOTFS","CONFIG","APP"};
static int die(const char *why){fprintf(stderr,"ANYKA_UPDATE refused/failed: %s (%s)\n",why,strerror(errno));return 1;}
static int io_at(int fd,void *data,size_t size,off_t pos,int writing)
{
 uint8_t *p=data;while(size){ssize_t n=writing?pwrite(fd,p,size,pos):pread(fd,p,size,pos);
 if(n<0&&errno==EINTR)continue;
 if(n<=0)return -1;
 p+=n;size-=(size_t)n;pos+=n;}return 0;
 
}
static int ram_fd(int fd)
{struct statfs fs;return fstatfs(fd,&fs)||((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)?-1:0;}
static int protected_file(int fd,off_t size)
{struct stat s;return fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid||s.st_nlink!=1||(s.st_mode&022)||s.st_size!=size?-1:0;}
static int hash_fd(int fd,uint32_t bytes,uint8_t out[64])
{crypto_blake2b_ctx c;uint8_t block[4096];crypto_blake2b_init(&c,64);
 for(uint32_t off=0;off<bytes;off+=sizeof block){size_t n=bytes-off;if(n>sizeof block)n=sizeof block;
 if(io_at(fd,block,n,off,0))return -1;
 crypto_blake2b_update(&c,block,n);}crypto_blake2b_final(&c,out);return 0;
 }
static int geometry(int fd,unsigned index)
{
 struct stat s;struct mtd_info_user m;char path[128],line[128],expected[128];
 if(fstat(fd,&s)||!S_ISCHR(s.st_mode)||major(s.st_rdev)!=90||minor(s.st_rdev)!=index*2||
 ioctl(fd,MEMGETINFO,&m)||m.type!=MTD_NORFLASH||m.size!=sizes[index]||m.erasesize!=ANYKA_APP_ERASE)return -1;
 
 snprintf(path,sizeof path,"/sys/class/mtd/mtd%u/name",index);FILE *f=fopen(path,"r");if(!f)return -1;
 
 int bad=!fgets(line,sizeof line,f);fclose(f);snprintf(expected,sizeof expected,"%s\n",names[index]);if(bad||strcmp(line,expected))return -1;
 
 snprintf(path,sizeof path,"/sys/class/mtd/mtd%u/offset",index);f=fopen(path,"r");if(!f)return -1;
 
 unsigned long offset=ULONG_MAX;bad=fscanf(f,"%lu",&offset)!=1;fclose(f);return bad||offset!=offsets[index]?-1:0;
}
static int accepted(uint64_t *out,int bootstrap)
{
 *out=0;int fd=open(STATE "/accepted",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
 if(fd<0)return errno==ENOENT&&bootstrap?0:-1;
 struct stat s;char text[32];if(fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid||(s.st_mode&022)||s.st_size<2||s.st_size>21){close(fd);return -1;
 }
 ssize_t n=read(fd,text,sizeof text-1);close(fd);if(n!=s.st_size||text[n-1]!='\n')return -1;
 text[n-1]=0;
 uint64_t value=0;for(ssize_t i=0;i<n-1;i++){unsigned d=(unsigned)(text[i]-'0');if(d>9||value>(UINT64_MAX-d)/10)return -1;
 value=value*10+d;}
 *out=value;return 0;
 
}
static int state_dir(void)
{
 if(mkdir("/etc/config/camera",0700)&&errno!=EEXIST)return -1;
 
 if(mkdir(STATE,0700)&&errno!=EEXIST)return -1;
 
 int fd=open(STATE,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat s;
 if(fd<0)return -1;
 if(fstat(fd,&s)||s.st_uid||(s.st_mode&022)){close(fd);return -1;
 }return fd;
}
static int boot_id(char text[38])
{
 int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
 
 ssize_t n=read(fd,text,38);close(fd);if(n!=37||text[36]!='\n')return -1;
 text[36]=0;return 0;
 
}
static int journal(int dir,const struct anyka_update_verified *v,const char *phase)
{
 char hex[129],text[512],boot[38];anyka_update_hex(hex,v->digest);if(boot_id(boot))return -1;
 
 int n=snprintf(text,sizeof text,"{\"schema\":1,\"board_id\":\"%s\",\"sequence\":%"PRIu64",\"payload_blake2b\":\"%s\",\"status\":\"%s\",\"install_boot_id\":\"%s\"}\n",ANYKA_UPDATE_BOARD,v->sequence,hex,phase,boot);
 int fd=openat(dir,"pending.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
 
 int bad=write(fd,text,(size_t)n)!=n||fsync(fd);if(close(fd))bad=1;
 if(!bad&&(renameat(dir,"pending.new",dir,"pending.json")||fsync(dir)))bad=1;
 return bad?-1:0;
}
static int quiesced(void)
{
 /* Trusted quiesce implementation writes current kernel boot ID only after
  * listener stop, watchdog disable acknowledgement and RAM runtime staging. */
 int directory=open("/tmp/camera-update",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat ds;
 if(directory<0)return -1;
 int invalid=fstat(directory,&ds)||ds.st_uid||(ds.st_mode&077)||ram_fd(directory);close(directory);
 if(invalid)return -1;
 char boot[64]={0},proof[64]={0};int a=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC);
 int b=open("/tmp/camera-update/quiesced",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
 if(a<0||b<0){if(a>=0)close(a);if(b>=0)close(b);return -1;
 }
 ssize_t n=read(a,boot,sizeof boot),m=read(b,proof,sizeof proof);
 int bad=n!=37||m!=n||memcmp(boot,proof,(size_t)n)||protected_file(b,37)||ram_fd(b);
 close(a);close(b);if(bad)return -1;
 
 DIR *procs=opendir("/proc");if(!procs)return -1;
 struct dirent *de;
 while((de=readdir(procs))){char *end;strtoul(de->d_name,&end,10);if(!*de->d_name||*end)continue;
 
  char path[96];snprintf(path,sizeof path,"/proc/%.20s/fd",de->d_name);DIR *fds=opendir(path);
  if(!fds){if(errno==ENOENT)continue;
 closedir(procs);return -1;
 }struct dirent *fe;
  while((fe=readdir(fds))){if(fe->d_name[0]=='.')continue;
 char link[160],target[PATH_MAX];
   snprintf(link,sizeof link,"%s/%.20s",path,fe->d_name);ssize_t got=readlink(link,target,sizeof target-1);
   if(got<0){if(errno==ENOENT)continue;
 closedir(fds);closedir(procs);return -1;
 }
   struct stat descriptor;
   if(stat(link,&descriptor)){if(errno==ENOENT)continue;closedir(fds);closedir(procs);return -1;}
   target[got]=0;if((S_ISCHR(descriptor.st_mode)&&major(descriptor.st_rdev)==10&&minor(descriptor.st_rdev)==130)||
                   !strcmp(target,"/dev/watchdog")||!strcmp(target,"/dev/watchdog0")){closedir(fds);closedir(procs);return -1;
 }
  }closedir(fds);
 }closedir(procs);return 0;
 
}
static volatile sig_atomic_t lease_broken;
static void break_lease(int signal_number){(void)signal_number;lease_broken=1;}
struct flash {int fd,source;};
static int erase_block(void *c,uint32_t off,uint32_t n)
{struct flash *f=c;if(lease_broken||fcntl(f->source,F_GETLEASE)!=F_RDLCK)return -1;
 struct erase_info_user e={off,n};return ioctl(f->fd,MEMERASE,&e);}
static int write_block(void *c,uint32_t off,const void *p,size_t n)
{return io_at(((struct flash*)c)->fd,(void*)p,n,off,1);}
static int read_block(void *c,uint32_t off,void *p,size_t n)
{return io_at(((struct flash*)c)->fd,p,n,off,0);}
static int progress(void *c,unsigned done,unsigned total)
{(void)c;printf("ANYKA_UPDATE blocks_verified=%u/%u\n",done,total);return fflush(stdout);}
static int read_receipt(int dir,const char *name,int health,struct anyka_update_receipt *out)
{
 int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
 
 struct stat s;if(fstat(fd,&s)||protected_file(fd,s.st_size)||s.st_size<1||s.st_size>1024){close(fd);return -1;
 }
 char text[1024];int bad=io_at(fd,text,(size_t)s.st_size,0,0);close(fd);
 return bad?-1:anyka_update_receipt_parse(text,(size_t)s.st_size,health,out);
}
static int confirm_boot(void)
{
 if(geteuid())return die("confirmation requires root");
 int dir=state_dir();if(dir<0)return die("trusted update state unavailable");
 int lock=openat(dir,"state.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);struct stat ls;
 if(lock<0||fstat(lock,&ls)||!S_ISREG(ls.st_mode)||ls.st_uid||(ls.st_mode&077)||ls.st_nlink!=1||flock(lock,LOCK_EX|LOCK_NB))return die("state confirmation lock");
 struct anyka_update_receipt pending,health;char boot[38],hex[129];uint8_t digest[64];uint64_t previous;
 if(read_receipt(dir,"pending.json",0,&pending)||read_receipt(dir,"boot-health.json",1,&health)||
    strcmp(pending.status,"written")||pending.sequence!=health.sequence||strcmp(pending.digest,health.digest)||
    boot_id(boot)||strcmp(boot,health.boot_id)||!strcmp(boot,pending.install_boot_id)||accepted(&previous,1)||
    pending.sequence<previous)return die("pending/new-boot health identity mismatch");
 int fd=open("/dev/mtd7",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
 if(fd<0||geometry(fd,7)||hash_fd(fd,ANYKA_APP_BYTES,digest))return die("confirmation APP readback");
 close(fd);
 anyka_update_hex(hex,digest);
 if(anyka_update_can_confirm(&pending,&health,boot,hex,previous))return die("current APP/boot health differs from pending");
 char text[32];int n=snprintf(text,sizeof text,"%"PRIu64"\n",pending.sequence);
 fd=openat(dir,"accepted.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
 if(fd<0)return die("accepted journal already exists");
 int bad=write(fd,text,(size_t)n)!=n||fsync(fd);if(close(fd))bad=1;
 if(!bad&&(renameat(dir,"accepted.new",dir,"accepted")||fsync(dir)))bad=1;
 if(bad)return die("accepted sequence persistence");
 
 /* Receipt retained as audit history; equality permits idempotent confirmation. */
 close(lock);close(dir);printf("ANYKA_UPDATE boot_confirmed=yes accepted=%"PRIu64"\n",pending.sequence);return 0;
 
}
int main(int argc,char **argv)
{
 if(argc==2&&!strcmp(argv[1],"--confirm-boot"))return confirm_boot();
 if(argc<3||argc>4||(strcmp(argv[1],"--verify")&&strcmp(argv[1],"--apply")&&strcmp(argv[1],"--adopt-current"))||
    (argc==4&&strcmp(argv[3],"--bootstrap")))return die("usage: installer --verify|--apply|--adopt-current RAM_CAPSULE [--bootstrap]");
 int apply=!strcmp(argv[1],"--apply"),bootstrap=argc==4;uint64_t baseline;
 if(geteuid()||accepted(&baseline,bootstrap))return die("root and valid accepted state required");
 int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
 size_t bytes=ANYKA_UPDATE_HEADER+ANYKA_APP_BYTES;
 if(fd<0||protected_file(fd,(off_t)bytes)||ram_fd(fd))return die("capsule must be trusted RAM regular file");
 struct stat source_stat;if(fstat(fd,&source_stat)||(source_stat.st_mode&0777)!=0400)return die("capsule must be sealed mode0400");
 struct sigaction action={0};action.sa_handler=break_lease;sigemptyset(&action.sa_mask);
 if(sigaction(SIGIO,&action,NULL)||fcntl(fd,F_SETOWN,getpid())||fcntl(fd,F_SETLEASE,F_RDLCK))return die("immutable RAM source read lease unavailable");
 uint8_t *data=mmap(NULL,bytes,PROT_READ,MAP_SHARED,fd,0);if(data==MAP_FAILED)return die("cannot map bounded leased RAM capsule");
 struct anyka_update_verified v;if(anyka_update_verify(data,bytes,anyka_update_public_key,baseline,&v)||
 lease_broken||fcntl(fd,F_GETLEASE)!=F_RDLCK)return die("signature, sequence, APP or source-lease validation");
 char hex[129];anyka_update_hex(hex,v.digest);printf("ANYKA_UPDATE verified board=%s sequence=%"PRIu64" payload_blake2b=%s\n",ANYKA_UPDATE_BOARD,v.sequence,hex);
 if(!strcmp(argv[1],"--adopt-current")){
  int current=open("/dev/mtd7",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);uint8_t digest[64];
  if(current<0||geometry(current,7)||hash_fd(current,ANYKA_APP_BYTES,digest))return die("adoption APP readback");
  close(current);if(crypto_verify64(digest,v.digest)){puts("ANYKA_UPDATE current_APP_differs=yes");return 3;}
  int state=state_dir();if(state<0)return die("adoption state unavailable");
  int lock=openat(state,"state.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);struct stat ls;
  if(lock<0||fstat(lock,&ls)||!S_ISREG(ls.st_mode)||ls.st_uid||(ls.st_mode&077)||ls.st_nlink!=1||flock(lock,LOCK_EX|LOCK_NB)||
     accepted(&baseline,bootstrap)||anyka_update_verify(data,bytes,anyka_update_public_key,baseline,&v))return die("adoption state/capsule lock");
  struct anyka_update_receipt old;char boot[38];
  if(!read_receipt(state,"pending.json",0,&old)&&!strcmp(old.status,"written")&&old.sequence==v.sequence&&
     !strcmp(old.digest,hex)&&!boot_id(boot)&&strcmp(old.install_boot_id,boot)){
   puts("ANYKA_UPDATE current_APP_already_pending_confirmation=yes");return 4;
  }
  if(lease_broken||fcntl(fd,F_GETLEASE)!=F_RDLCK||journal(state,&v,"written"))return die("adoption pending receipt");
  close(lock);close(state);munmap(data,bytes);close(fd);
  puts("ANYKA_UPDATE current_APP_adopted=yes flash_written=no reboot_required=yes");return 0;
 }
 if(!apply){munmap(data,bytes);close(fd);return 0;
 }
 struct utsname u;if(uname(&u)||strcmp(u.release,"4.4.192V2.1")||strncmp(u.machine,"arm",3))return die("kernel/architecture mismatch");
 int self=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);if(self<0||ram_fd(self))return die("installer must execute from RAM");close(self);
 int lock=open("/tmp/camera-update/install.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
 struct stat ls;if(lock<0||fstat(lock,&ls)||!S_ISREG(ls.st_mode)||ls.st_uid||(ls.st_mode&077)||ls.st_nlink!=1||ram_fd(lock)||flock(lock,LOCK_EX|LOCK_NB))return die("exclusive RAM install lock");
 if(quiesced())return die("quiesce/watchdog/RAM ownership proof absent");
 int part[8];uint8_t before[6][64],after[64];
 for(unsigned i=0;i<8;i++){char path[32];snprintf(path,sizeof path,"/dev/mtd%u",i);part[i]=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
  if(part[i]<0||geometry(part[i],i))return die("exact MTD geometry/identity required");}
 for(unsigned i=3;i<=5;i++)if(hash_fd(part[i],sizes[i],after)||crypto_verify64(after,anyka_update_immutable_digest[i-3]))return die("DTB/kernel/rootfs board revision differs");
 /* Normal unmount is mandatory. It fails while any old APP mapped/open file
  * remains; lazy detach would conceal unsafe dependencies and is forbidden. */
 if(umount("/usr")&&errno!=EINVAL)return die("APP mount still busy");
 FILE *mounts=fopen("/proc/mounts","r");if(!mounts)return die("mount audit unavailable");char line[512];
 while(fgets(line,sizeof line,mounts))if(strstr(line,"mtdblock7")||strstr(line," /usr "))return die("APP remains mounted");
 fclose(mounts);
 int state=state_dir();if(state<0)return die("cannot open trusted update state");
 int state_lock=openat(state,"state.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);struct stat ss;
 if(state_lock<0||fstat(state_lock,&ss)||!S_ISREG(ss.st_mode)||ss.st_uid||(ss.st_mode&077)||ss.st_nlink!=1||
    flock(state_lock,LOCK_EX|LOCK_NB)||accepted(&baseline,bootstrap)||
    anyka_update_verify(data,bytes,anyka_update_public_key,baseline,&v)||journal(state,&v,"installing"))return die("cannot lock/persist pending journal");
 for(unsigned i=0;i<6;i++)if(hash_fd(part[i],sizes[i],before[i]))return die("protected partition snapshot");
 struct flash flash={open("/dev/mtd7",O_RDWR|O_NOFOLLOW|O_CLOEXEC),fd};
 if(flash.fd<0||geometry(flash.fd,7)||quiesced())return die("APP write descriptor/isolation mismatch");
 struct anyka_update_flash backend={&flash,erase_block,write_block,read_block,progress};
 if(anyka_update_apply(data,bytes,anyka_update_public_key,baseline,&backend))return die("APP erase/write/readback failed; keep power and recover");
 if(close(flash.fd))return die("APP descriptor close failed");
 if(hash_fd(part[7],ANYKA_APP_BYTES,after)||crypto_verify64(after,v.digest))return die("full APP readback mismatch");
 for(unsigned i=0;i<6;i++)if(hash_fd(part[i],sizes[i],after)||crypto_verify64(before[i],after))return die("immutable protected partition changed");
 if(journal(state,&v,"written"))return die("APP verified but completion journal failed");
 close(state_lock);close(state);for(unsigned i=0;i<8;i++)close(part[i]);close(lock);munmap(data,bytes);close(fd);
 puts("ANYKA_UPDATE written_verified=yes boot_confirmed=no accepted_advanced=no reboot_required=yes");return 0;
 
}
