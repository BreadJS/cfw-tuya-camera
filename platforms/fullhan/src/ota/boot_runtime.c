#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "boot_runtime.h"
#include "watchdog_clock_probe.h"
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

#define RAMFS_MAGIC 0x858458f6UL
#define TMPFS_MAGIC 0x01021994UL
#define JFFS2_MAGIC 0x72b6UL
#define PROC_MAGIC 0x9fa0UL
#define SYSFS_MAGIC 0x62656572UL
_Static_assert(sizeof(struct mtd_info_user)==32,"known MTD info ABI");
_Static_assert(MEMGETINFO==0x80204d01UL,"known read-only MTD info ioctl");
_Static_assert(MEMGETREGIONCOUNT==0x80044d07UL,"known read-only region ioctl");
_Static_assert(sizeof(off_t)==8,"64-bit positioned reads");

static int fail(char *e,size_t n,const char *s)
{if(n)snprintf(e,n,"%s",s);return -1;}
static int memory_type(unsigned long type)
{return type==RAMFS_MAGIC||type==TMPFS_MAGIC;}
static int ascii_space(unsigned char c)
{return c==' '||c=='\t'||c=='\r'||c=='\n';}
static int token(const char *p,size_t n,const char *s)
{return n==strlen(s)&&!memcmp(p,s,n);}
static int prefix(const char *p,size_t n,const char *s)
{return n>=strlen(s)&&!memcmp(p,s,strlen(s));}
int camera_boot_cmdline_is_installed(const char *text,size_t length)
{
    if(!text||!length||length>4096)return 0;
    unsigned roots=0,types=0;
    for(size_t at=0;at<length;){
        while(at<length&&ascii_space((unsigned char)text[at]))at++;
        size_t start=at;
        while(at<length&&!ascii_space((unsigned char)text[at])){
            unsigned char c=(unsigned char)text[at++];
            if(c<33||c>126||c=='"'||c=='\'')return 0;
        }
        size_t n=at-start;if(!n)continue;const char *p=text+start;
        if(prefix(p,n,"root=")){
            if(++roots!=1||!token(p,n,"root=/dev/mtdblock4"))return 0;
        }else if(prefix(p,n,"rootfstype=")){
            if(++types!=1||!token(p,n,"rootfstype=squashfs"))return 0;
        }else if(prefix(p,n,"init=")||token(p,n,"init")||prefix(p,n,"rdinit=")||token(p,n,"rdinit")||prefix(p,n,"camera_sd_test")||
                 prefix(p,n,"camera_recovery")||prefix(p,n,"recovery")||
                 token(p,n,"rescue")||prefix(p,n,"rescue=")||prefix(p,n,"emergency")||prefix(p,n,"rd.")||
                 prefix(p,n,"systemd.unit=")||prefix(p,n,"init_args=")||token(p,n,"single")||
                 token(p,n,"s")||token(p,n,"S")||token(p,n,"rw")||p[0]=='-'||
                 (n==1&&p[0]>='0'&&p[0]<='6'))return 0;
    }
    return roots==1;
}
static int hex(unsigned char c)
{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
int camera_boot_parse_id(const char *text,size_t length,uint8_t out[16])
{
    if(!text||!out||(length!=36&&length!=37)||(length==37&&text[36]!='\n'))return -1;
    uint8_t parsed[16],all=0;unsigned used=0;
    for(unsigned i=0;i<36;){
        if(i==8||i==13||i==18||i==23){if(text[i++]!='-')return -1;continue;}
        int a=hex((unsigned char)text[i++]),b=hex((unsigned char)text[i++]);if(a<0||b<0||used>=16)return -1;
        parsed[used]=(uint8_t)(a*16+b);all|=parsed[used++];
    }
    if(used!=16||!all)return -1;
    memcpy(out,parsed,sizeof parsed);return 0;
}
static int private_directory(const char *path)
{
    if(!path||path[0]!='/'){errno=EINVAL;return -1;}
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;
    if(fstat(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)){close(fd);errno=EPERM;return -1;}
    return fd;
}
int camera_boot_confirmation_needed(const char *path)
{
    int dir=private_directory(path);if(dir<0)return errno==ENOENT?0:-1;
    struct stat st;int rc=fstatat(dir,OTA_BOOT_JOURNAL_NAME,&st,AT_SYMLINK_NOFOLLOW);
    int saved=errno;if(close(dir))return -1;
    if(rc)return saved==ENOENT?0:-1;
    if(!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1||st.st_size!=OTA_BOOT_JOURNAL_BYTES){errno=EPERM;return -1;}
    return 1;
}
/* Bounded reads for immutable kernel pseudo-files. No size assumption: proc
 * attributes commonly report st_size0. */
static int system_text(const char *path,unsigned long magic,char *out,size_t cap,size_t *length)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;struct statfs fs;int rc=-1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&022)||fstatfs(fd,&fs)||(unsigned long)fs.f_type!=magic)goto done;
    size_t used=0;
    while(used<cap){ssize_t n=read(fd,out+used,cap-used);if(n<0&&errno==EINTR)continue;if(n<0)goto done;if(!n){*length=used;rc=0;goto done;}used+=(size_t)n;}
done:if(close(fd))rc=-1;return rc;
}
static int ram_path(const char *path,dev_t *device)
{
    struct stat st;struct statfs fs;
    if(stat(path,&st)||statfs(path,&fs)||!memory_type((unsigned long)fs.f_type))return -1;
    if(device)*device=st.st_dev;
    return 0;
}
static int provenance(dev_t root_device)
{
    int fd=open("/.camera-boot-origin",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    struct stat st;struct statfs fs;char data[13];int rc=-1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&07777)!=0400||st.st_nlink!=1||st.st_dev!=root_device||st.st_size!=12||fstatfs(fd,&fs)||!memory_type((unsigned long)fs.f_type))goto done;
    ssize_t n;do{n=read(fd,data,sizeof data);}while(n<0&&errno==EINTR);
    if(n==12&&!memcmp(data,"NOR_ROOT_V1\n",12))rc=0;
done:if(close(fd))rc=-1;return rc;
}
struct devices {
    int fd[2];struct stat identity[2];struct ota_mtd_map map;
};
static const char *const device_path[2]={"/dev/mtd3","/dev/mtd4"};
static int character(const struct stat *st,unsigned index)
{return S_ISCHR(st->st_mode)&&st->st_uid==0&&!(st->st_mode&022)&&major(st->st_rdev)==90&&minor(st->st_rdev)==index*2;}
static int same(const struct stat *a,const struct stat *b)
{return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_rdev==b->st_rdev&&a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_gid==b->st_gid;}
static int private_lock(int directory,int fd)
{
    struct stat a,b;
    return fstat(fd,&a)||!S_ISREG(a.st_mode)||a.st_uid!=0||(a.st_mode&077)||a.st_nlink!=1||
        fstatat(directory,"lock",&b,AT_SYMLINK_NOFOLLOW)||!same(&a,&b)||b.st_nlink!=1?-1:0;
}
static int named_identity(struct devices *d,unsigned slot)
{
    struct stat opened,named;int flags=fcntl(d->fd[slot],F_GETFL);
    return flags<0||(flags&O_ACCMODE)!=O_RDONLY||fstat(d->fd[slot],&opened)||
        lstat(device_path[slot],&named)||!character(&opened,slot+3)||!character(&named,slot+3)||
        !same(&opened,&named)||!same(&opened,&d->identity[slot])?-1:0;
}
static int attribute(unsigned index,const char *field,const char *expected)
{
    char path[96],value[128];size_t length;
    int n=snprintf(path,sizeof path,"/sys/class/mtd/mtd%u/%s",index,field);
    return n<0||(size_t)n>=sizeof path||system_text(path,SYSFS_MAGIC,value,sizeof value,&length)||length!=strlen(expected)||memcmp(value,expected,length)?-1:0;
}
static int info_ok(unsigned index,const struct mtd_info_user *info)
{
    return (index==3||index==4)&&info->type==MTD_NORFLASH&&
        info->size==(index==3?OTA_KERNEL_LIMIT:OTA_ROOTFS_LIMIT)&&info->erasesize==OTA_ERASE_SIZE&&
        info->writesize==1&&!info->oobsize&&(info->flags&MTD_BIT_WRITEABLE)&&
        !(info->flags&(MTD_NO_ERASE|MTD_POWERUP_LOCK));
}
static int read_ioctl(int fd,unsigned long request,void *data)
{int rc;do{rc=ioctl(fd,request,data);}while(rc<0&&errno==EINTR);return rc;}
static int target(struct devices *d,unsigned slot)
{
    unsigned index=slot+3;struct mtd_info_user info={0};int regions=-1;
    if(named_identity(d,slot)||read_ioctl(d->fd[slot],MEMGETINFO,&info)||read_ioctl(d->fd[slot],MEMGETREGIONCOUNT,&regions)||regions||!info_ok(index,&info))return -1;
    char text[64];const struct ota_mtd_partition *p=&d->map.part[index];
    snprintf(text,sizeof text,"%s\n",p->name);if(attribute(index,"name",text))return -1;
    snprintf(text,sizeof text,"90:%u\n",index*2);if(attribute(index,"dev",text))return -1;
    snprintf(text,sizeof text,"%u\n",p->size);if(attribute(index,"size",text))return -1;
    snprintf(text,sizeof text,"%u\n",p->flash_offset);return attribute(index,"offset",text);
}
static int map_unchanged(struct devices *d)
{
    struct ota_mtd_map map;char error[128];
    return ota_mtd_read("/proc/mtd",&map,error,sizeof error)||ota_mtd_validate(&map,error,sizeof error)||!ota_mtd_equal(&map,&d->map)?-1:0;
}
static ssize_t installed_read(void *opaque,unsigned mtd,uint32_t offset,void *data,size_t size)
{
    struct devices *d=opaque;
    if(mtd<3||mtd>4){errno=EINVAL;return -1;}
    unsigned slot=mtd-3;uint32_t limit=d->map.part[mtd].size;
    if(!size||offset>=limit||size>limit-offset||named_identity(d,slot)){errno=EIO;return -1;}
    ssize_t n=pread(d->fd[slot],data,size,(off_t)offset);
    /* Revalidate full geometry/map before returning the last selected byte;
     * core never commits accepted until all selected partitions are complete. */
    if(n>0&&(uint64_t)offset+(uint64_t)n==limit&&(target(d,slot)||map_unchanged(d))){errno=EIO;return -1;}
    return n;
}
int camera_boot_confirm_installed(const char *key_path,const char *state_path,int healthy,
                                  struct ota_boot_result *result,char *error,size_t size)
{
    if(!result)return fail(error,size,"missing confirmation result");
    if(error&&size)error[0]=0;
    memset(result,0,sizeof *result);
    int needed=camera_boot_confirmation_needed(state_path);if(!needed)return 0;
    if(needed<0)return fail(error,size,"unsafe install journal metadata");
    if(geteuid()!=0)return fail(error,size,"confirmation requires trusted root service");
    struct utsname kernel;dev_t root_device;struct stat config;struct statfs configfs;
    if(uname(&kernel)||strcmp(kernel.release,"4.9.129")||strcmp(kernel.version,"#55 PREEMPT Sat Dec 11 11:44:24 CST 2021")||ram_path("/",&root_device)||ram_path("/proc/1/exe",NULL)||ram_path("/proc/1/root",NULL)||provenance(root_device))return fail(error,size,"installed NOR-to-RAM boot provenance not established");
    if(stat("/etc/conf",&config)||statfs("/etc/conf",&configfs)||(unsigned long)configfs.f_type!=JFFS2_MAGIC||major(config.st_dev)!=31||minor(config.st_dev)!=5)return fail(error,size,"configuration is not persistent camera JFFS2");
    char text[4097];size_t length;uint8_t boot[16],key[32];
    if(system_text("/proc/cmdline",PROC_MAGIC,text,sizeof text,&length)||!camera_boot_cmdline_is_installed(text,length)||system_text("/proc/sys/kernel/random/boot_id",PROC_MAGIC,text,sizeof text,&length)||camera_boot_parse_id(text,length,boot))return fail(error,size,"invalid installed boot arguments/identity");
    struct camera_watchdog_clock_report watchdog;
    if(camera_watchdog_clock_inspect(&watchdog,error,size))return -1;
    if(ota_read_public_key(key_path,key,1,error,size))return -1;
    int dir=private_directory(state_path),lock=-1,rc=-1;struct devices devices={.fd={-1,-1}};
    if(dir<0)return fail(error,size,"cannot pin private confirmation directory");
    struct stat state;
    if(fstat(dir,&state)||state.st_dev!=config.st_dev)goto done;
    lock=openat(dir,"lock",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(lock<0||private_lock(dir,lock)||flock(lock,LOCK_EX|LOCK_NB))goto done;
    if(ota_mtd_read("/proc/mtd",&devices.map,error,size)||ota_mtd_validate(&devices.map,error,size))goto done;
    for(unsigned slot=0;slot<2;slot++){
        devices.fd[slot]=open(device_path[slot],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(devices.fd[slot]<0||fstat(devices.fd[slot],&devices.identity[slot])||target(&devices,slot))goto done;
    }
    struct ota_boot_reader reader={.context=&devices,.read=installed_read};
    rc=ota_boot_confirm(dir,lock,key,boot,1,healthy,&reader,NULL,NULL,result,error,size);
done:
    {int bad=0;for(unsigned slot=0;slot<2;slot++)if(devices.fd[slot]>=0&&close(devices.fd[slot]))bad=1;
    if(lock>=0&&close(lock))bad=1;
    if(close(dir))bad=1;
    if(bad)return fail(error,size,"confirmation descriptor cleanup failed");}
    if(rc<0&&(!error||!size||!error[0]))return fail(error,size,"cannot pin confirmation state or read-only MTD geometry");
    return rc;
}
