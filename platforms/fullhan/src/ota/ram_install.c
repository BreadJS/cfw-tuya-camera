/* Single-process production installer. No fork, MTD dup, reboot or commit. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "ram_install.h"
#include "boot_confirm.h"
#include "watchdog_clock_probe.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

struct install {
    struct ota_apply_plan *plan;
    struct ota_linux_mtd mtd;
    struct ram_install_result *result;
    int report_fd;
    unsigned reported_blocks;
    const char *phase;
    char *error;size_t error_size;
#ifdef CAMERA_RAM_INSTALL_TEST
    const struct ram_install_test_ops *test;
#endif
};
static int fail(char *error,size_t size,const char *why)
{if(size)snprintf(error,size,"%s",why);return -1;}
static int memory_fs(unsigned long type)
{return type==0x01021994UL||type==0x858458f6UL;}
static int ram_path(const char *path)
{struct statfs fs;return !statfs(path,&fs)&&memory_fs((unsigned long)fs.f_type);}
static int private_dir(int fd,int memory)
{
    struct stat st;struct statfs fs;
    return fd<0||fstat(fd,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||
        (st.st_mode&07777)!=0700||fstatfs(fd,&fs)||(memory&&!memory_fs((unsigned long)fs.f_type))?-1:0;
}
static int write_all(int fd,const void *data,size_t bytes)
{
    const unsigned char *p=data;
    while(bytes){ssize_t n=write(fd,p,bytes);if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        p+=n;bytes-=(size_t)n;}
    return 0;
}
static int progress(struct install *s,const char *phase)
{
    if(private_dir(s->report_fd,1))return -1;
    struct stat old;
    if(!fstatat(s->report_fd,"install-progress.new",&old,AT_SYMLINK_NOFOLLOW)){
        if(!S_ISREG(old.st_mode)||old.st_uid!=geteuid()||(old.st_mode&077)||old.st_nlink!=1||
           old.st_size<0||old.st_size>768||unlinkat(s->report_fd,"install-progress.new",0))return -1;
    }else if(errno!=ENOENT)return -1;
    char text[768];struct ram_install_result *r=s->result;
    int n=snprintf(text,sizeof text,"{\"phase\":\"%s\",\"sequence\":%"PRIu64
        ",\"blocks_completed\":%u,\"blocks_total\":%u,\"bytes_erased\":%"PRIu64
        ",\"bytes_written\":%"PRIu64",\"bytes_read_back\":%"PRIu64
        ",\"journal_prepared\":%s,\"may_have_changed\":%s,\"verified_written\":%s,"
        "\"reboot_pending\":%s,\"boot_confirmed\":false,\"accepted_advanced\":false,\"applied\":false}\n",
        phase,r->sequence,r->progress.blocks_completed,s->plan->block_count,
        r->progress.bytes_erased,r->progress.bytes_written,r->progress.bytes_read_back,
        r->journal_prepared?"true":"false",r->may_have_changed?"true":"false",
        r->verified_written?"true":"false",r->verified_written?"true":"false");
    if(n<0||(size_t)n>=sizeof text)return -1;
    int fd=openat(s->report_fd,"install-progress.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    int bad=write_all(fd,text,(size_t)n)||fsync(fd);
    if(close(fd))bad=1;
    if(!bad&&(renameat(s->report_fd,"install-progress.new",s->report_fd,"install-progress.json")||fsync(s->report_fd)))bad=1;
    if(bad){unlinkat(s->report_fd,"install-progress.new",0);return -1;}
    s->reported_blocks=r->progress.blocks_completed;s->phase=phase;return 0;
}
static int selected(dev_t dev)
{return major(dev)==31&&(minor(dev)==3||minor(dev)==4);}
static int digits(const char *s)
{if(!*s)return 0;for(;*s;s++)if(*s<'0'||*s>'9')return 0;return 1;}
static int allowed_fd(struct install *s,unsigned long pid,int fd,const struct stat *st)
{
    if(pid!=(unsigned long)getpid()||!s->mtd.active)return 0;
    for(unsigned i=0;i<2;i++)if(fd==s->mtd.fd[i]||fd==s->mtd.sync_fd[i]){
        const struct stat *known=&s->mtd.identity[i];
        return S_ISCHR(st->st_mode)&&major(st->st_rdev)==90&&minor(st->st_rdev)==(i+3)*2&&
            st->st_dev==known->st_dev&&st->st_ino==known->st_ino&&st->st_rdev==known->st_rdev&&
            st->st_mode==known->st_mode&&st->st_uid==known->st_uid&&st->st_gid==known->st_gid;
    }
    return 0;
}
static int process(struct install *s,unsigned long pid)
{
    char path[128],line[4096];snprintf(path,sizeof path,"/proc/%lu/maps",pid);
    FILE *file=fopen(path,"r");if(!file)return errno==ENOENT?0:-1;
    unsigned lines=0;int bad=0;
    while(fgets(line,sizeof line,file)){
        unsigned ma,mi;lines++;
        if(!strchr(line,'\n')||sscanf(line,"%*s %*s %*s %x:%x",&ma,&mi)!=2||
           (ma==31&&(mi==3||mi==4))){bad=1;break;}
    }
    if(ferror(file))bad=1;
    fclose(file);snprintf(path,sizeof path,"/proc/%lu/exe",pid);struct stat st;
    if(stat(path,&st)){
        if(errno!=ENOENT)return -1;
        if(!lines)return 0;
        snprintf(path,sizeof path,"/proc/%lu",pid);
        if(stat(path,&st)&&errno==ENOENT)return 0;
        snprintf(path,sizeof path,"/proc/%lu/status",pid);file=fopen(path,"r");
        if(!file)return errno==ENOENT?0:-1;
        int zombie=0;
        while(fgets(line,sizeof line,file))if(!strncmp(line,"State:",6)){
            const char *p=line+6;while(*p==' '||*p=='\t')p++;zombie=*p=='Z';break;
        }
        fclose(file);return zombie?0:-1;
    }
    if(bad||!ram_path(path))return -1;
    snprintf(path,sizeof path,"/proc/%lu/root",pid);if(!ram_path(path))return -1;
    snprintf(path,sizeof path,"/proc/%lu/cwd",pid);if(stat(path,&st)||selected(st.st_dev))return -1;
    snprintf(path,sizeof path,"/proc/%lu/fd",pid);DIR *dir=opendir(path);if(!dir)return -1;
    struct dirent *entry;
    while((entry=readdir(dir))){
        if(!digits(entry->d_name))continue;
        char fdpath[192];int n=snprintf(fdpath,sizeof fdpath,"%s/%s",path,entry->d_name);
        if(n<0||(size_t)n>=sizeof fdpath){bad=1;break;}
        if(stat(fdpath,&st)){if(errno!=ENOENT)bad=1;continue;}
        if(selected(st.st_dev)||(S_ISBLK(st.st_mode)&&major(st.st_rdev)==31)||
           (S_ISCHR(st.st_mode)&&major(st.st_rdev)==10&&minor(st.st_rdev)==130)||
           (S_ISCHR(st.st_mode)&&major(st.st_rdev)==90&&!allowed_fd(s,pid,atoi(entry->d_name),&st))){bad=1;break;}
    }
    closedir(dir);return bad?-1:0;
}
static int detached(struct install *s)
{
    FILE *file=fopen("/proc/self/mountinfo","r");if(!file)return -1;
    char line[4096];unsigned a,b,ma,mi;int bad=0;
    while(fgets(line,sizeof line,file))if(!strchr(line,'\n')||
        sscanf(line,"%u %u %u:%u",&a,&b,&ma,&mi)!=4||(ma==31&&(mi==3||mi==4))){bad=1;break;}
    if(ferror(file))bad=1;
    fclose(file);if(bad)return -1;
    DIR *proc=opendir("/proc");if(!proc)return -1;struct dirent *entry;
    while((entry=readdir(proc)))if(digits(entry->d_name)&&process(s,strtoul(entry->d_name,NULL,10))){bad=1;break;}
    closedir(proc);return bad?-1:0;
}
static int single_thread(void)
{
    DIR *dir=opendir("/proc/self/task");if(!dir)return -1;
    unsigned count=0;struct dirent *e;while((e=readdir(dir)))if(digits(e->d_name))count++;
    closedir(dir);return count==1?0:-1;
}
static int cmdline(char *args)
{
#ifdef CAMERA_INITIAL_DEPLOY
    unsigned roots=0,types=0,inits=0,trials=0,memory=0,consoles=0;char *save=NULL;
    for(char *p=strtok_r(args," \t\r\n",&save);p;p=strtok_r(NULL," \t\r\n",&save)){
        if(!strncmp(p,"root=",5)){if(strcmp(p,"root=/dev/mmcblk0p1"))return -1;roots++;}
        if(!strncmp(p,"rootfstype=",11)){if(strcmp(p,"rootfstype=vfat"))return -1;types++;}
        if(!strncmp(p,"init=",5)){if(strcmp(p,"init=/RAMINIT"))return -1;inits++;}
        if(!strncmp(p,"camera_sd_test",14)){if(strcmp(p,"camera_sd_test=1"))return -1;trials++;}
        if(!strncmp(p,"mem=",4)){if(strcmp(p,"mem=36M"))return -1;memory++;}
        if(!strncmp(p,"console=",8)){if(strcmp(p,"console=ttyS0,115200"))return -1;consoles++;}
        if(!strncmp(p,"camera_recovery",15)||!strncmp(p,"rdinit=",7))return -1;
    }
    return roots==1&&types==1&&inits==1&&trials==1&&memory==1&&consoles==1?0:-1;
#else
    unsigned roots=0,types=0;char *save=NULL;
    for(char *p=strtok_r(args," \t\r\n",&save);p;p=strtok_r(NULL," \t\r\n",&save)){
        if(!strncmp(p,"root=",5)){if(strcmp(p,"root=/dev/mtdblock4"))return -1;roots++;}
        if(!strncmp(p,"rootfstype=",11)){if(strcmp(p,"rootfstype=squashfs"))return -1;types++;}
        if(!strncmp(p,"camera_sd_test",14)||!strncmp(p,"camera_recovery",15)||
           !strncmp(p,"rdinit=",7)||(!strncmp(p,"init=",5)&&strcmp(p,"init=/sbin/init")))return -1;
    }
    return roots==1&&types==1?0:-1;
#endif
}
static int origin(void)
{
    int fd=open("/.camera-boot-origin",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    static const char expected_origin[]=
#ifdef CAMERA_INITIAL_DEPLOY
        "RECOVERY_ROOT_V1\n";
#else
        "NOR_ROOT_V1\n";
#endif
    struct stat st;struct statfs fs;char value[sizeof expected_origin-1];
    int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||st.st_nlink!=1||
        (st.st_mode&07777)!=0400||st.st_size!=(off_t)sizeof value||fstatfs(fd,&fs)||!memory_fs((unsigned long)fs.f_type)||
        pread(fd,value,sizeof value,0)!=(ssize_t)sizeof value||memcmp(value,expected_origin,sizeof value);
    if(close(fd))bad=1;
    if(bad)return -1;
    fd=open("/proc/cmdline",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    char args[2048];ssize_t n=read(fd,args,sizeof args-1);int closed=close(fd);
    if(n<=0||n>=(ssize_t)sizeof args-1||closed)return -1;
    args[n]=0;return cmdline(args);
}
static int release49(void)
{
    /* Exact pinned stock mtdchar_close: writable final release invokes the
     * optional _sync callback before put_mtd_device/free; unchanged by patch. */
    static const unsigned char expected[80]={
        0x70,0x40,0x2d,0xe9,0x01,0x40,0xa0,0xe1,0x78,0x60,0x91,0xe5,0x20,0x30,0x91,0xe5,
        0x02,0x00,0x13,0xe3,0x00,0x50,0x96,0xe5,0x04,0x00,0x00,0x0a,0x98,0x30,0x95,0xe5,
        0x00,0x00,0x53,0xe3,0x01,0x00,0x00,0x0a,0x05,0x00,0xa0,0xe1,0x33,0xff,0x2f,0xe1,
        0x05,0x00,0xa0,0xe1,0x00,0x50,0xa0,0xe3,0x43,0xf2,0xff,0xeb,0x06,0x00,0xa0,0xe1,
        0x78,0x50,0x84,0xe5,0x65,0x94,0xfa,0xeb,0x05,0x00,0xa0,0xe1,0x70,0x80,0xbd,0xe8};
    int fd=open("/dev/mem",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    struct stat st;int bad=fstat(fd,&st)||!S_ISCHR(st.st_mode)||st.st_uid!=0||
        major(st.st_rdev)!=1||minor(st.st_rdev)!=1;
    void *page=bad?MAP_FAILED:mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,(off_t)UINT64_C(0xa021a000));
    if(page==MAP_FAILED)bad=1;
    else {const volatile unsigned char *p=(const volatile unsigned char *)page+0xa90;
        for(unsigned i=0;i<sizeof expected;i++)if(p[i]!=expected[i]){bad=1;break;}
        if(munmap(page,4096))bad=1;}
    if(close(fd))bad=1;
    return bad?-1:0;
}
static int runtime(struct install *s)
{
    struct utsname kernel;
    if(geteuid()!=0||sysconf(_SC_PAGESIZE)!=4096||uname(&kernel)||strcmp(kernel.release,"4.9.129")||
       strcmp(kernel.version,"#55 PREEMPT Sat Dec 11 11:44:24 CST 2021")||origin()||
       !ram_path("/")||!ram_path("/proc/1/exe")||!ram_path("/proc/1/root")||
       !ram_path("/proc/self/exe")||!ram_path("/proc/self/cwd")||single_thread())return -1;
    if(detached(s))return -1;
    struct camera_watchdog_clock_report watchdog;char reason[128];
    return camera_watchdog_clock_inspect(&watchdog,reason,sizeof reason)||release49()?-1:0;
}
static int environment(struct install *s,enum ota_mtd_phase phase)
{
#ifdef CAMERA_RAM_INSTALL_TEST
    if(s->test)return s->test->environment(s->test->context,s->plan,phase);
#else
    (void)phase;
#endif
    struct stat config,state,held,named;struct statfs fs;
    if(runtime(s))return -1;
    if(stat("/etc/conf",&config)||statfs("/etc/conf",&fs)||(unsigned long)fs.f_type!=0x72b6UL||
       major(config.st_dev)!=31||minor(config.st_dev)!=5||fstat(s->plan->state_dir,&state)||
       state.st_dev!=config.st_dev||fstatfs(s->plan->state_dir,&fs)||(unsigned long)fs.f_type!=0x72b6UL||
       private_dir(s->plan->state_dir,0)||private_dir(s->plan->stage_dir,1)||private_dir(s->report_fd,1))return -1;
    if(fstat(s->plan->lock_fd,&held)||fstatat(s->plan->state_dir,"lock",&named,AT_SYMLINK_NOFOLLOW)||
       !S_ISREG(held.st_mode)||!S_ISREG(named.st_mode)||held.st_uid!=0||named.st_uid!=0||
       (held.st_mode&077)||(named.st_mode&077)||held.st_nlink!=1||named.st_nlink!=1||
       held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||
       flock(s->plan->lock_fd,LOCK_EX|LOCK_NB))return -1;
    char reason[128];
    struct ota_mtd_map actual;
    return ota_mtd_read("/proc/mtd",&actual,reason,sizeof reason)||ota_mtd_validate(&actual,reason,sizeof reason)||
        !ota_mtd_equal(&actual,&s->plan->map)?-1:0;
}
#ifdef CAMERA_INITIAL_DEPLOY
int ram_install_initial_runtime_check(char *error,size_t size)
{
    struct ota_apply_plan plan;struct install s;memset(&plan,0,sizeof plan);memset(&s,0,sizeof s);
    s.plan=&plan;s.mtd.fd[0]=s.mtd.fd[1]=s.mtd.sync_fd[0]=s.mtd.sync_fd[1]=-1;
    if(runtime(&s)||ota_mtd_read("/proc/mtd",&plan.map,error,size)||ota_mtd_validate(&plan.map,error,size))
        return fail(error,size,"initial recovery RAM/kernel/watchdog/isolation check refused");
    return 0;
}
#endif
static int lease(void *opaque,const struct ota_apply_plan *plan,
                 enum ota_mtd_phase phase,enum ota_mtd_sync mode)
{
    struct install *s=opaque;
    if(plan!=s->plan||mode!=OTA_MTD_SYNC_RELEASE49||environment(s,phase)){
        fail(s->error,s->error_size,"trusted RAM isolation or watchdog/sync proof failed");return -1;
    }
    if(phase==OTA_MTD_ERASE&&!s->result->may_have_changed){
        /* Publish the conservative dirty status BEFORE the first ioctl can
         * mutate flash, rather than waiting for the first verified block. */
        s->result->may_have_changed=1;
        if(progress(s,"applying")){
            fail(s->error,s->error_size,"RAM progress publication failed before erase");return -1;
        }
    }
    if(s->result->progress.blocks_completed!=s->reported_blocks&&progress(s,"applying")){
        fail(s->error,s->error_size,"RAM progress publication failed");return -1;
    }
    return 0;
}
static int boot_id(uint8_t out[16])
{
    int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    char data[40];ssize_t n=read(fd,data,sizeof data);int bad=close(fd);
    if(bad||n!=37||data[36]!='\n')return -1;
    unsigned used=0,nonzero=0;
    for(unsigned i=0;i<36;){
        if(i==8||i==13||i==18||i==23){if(data[i++]!='-')return -1;continue;}
        unsigned value=0;
        for(unsigned j=0;j<2;j++){unsigned c=(unsigned char)data[i++];
            if(c>='0'&&c<='9')c-='0';else if(c>='a'&&c<='f')c=c-'a'+10;else return -1;
            value=value*16+c;}
        if(used>=16)return -1;
        out[used++]=(uint8_t)value;nonzero|=value;
    }
    return used==16&&nonzero?0:-1;
}
static int execute(struct install *s,const uint8_t current_boot[16])
{
    struct ota_apply_backend backend;struct ota_mtd_lease isolation={s,lease};
    if(progress(s,"checking")||environment(s,OTA_MTD_OPEN))
        return fail(s->error,s->error_size,"production NOR-origin/RAM isolation preconditions refused");
#ifdef CAMERA_RAM_INSTALL_TEST
    int prepared=s->test&&s->test->prepare?s->test->prepare(s->test->context,s->plan,current_boot,s->error,s->error_size):
        ota_boot_prepare(s->plan,current_boot,NULL,NULL,s->error,s->error_size);
#else
    int prepared=ota_boot_prepare(s->plan,current_boot,NULL,NULL,s->error,s->error_size);
#endif
    if(prepared)return -1;
    s->result->journal_prepared=1;
    if(progress(s,"prepared"))return fail(s->error,s->error_size,"cannot publish prepared status");
    const struct ota_mtd_syscalls *sys=NULL;void *context=NULL;
#ifdef CAMERA_RAM_INSTALL_TEST
    if(s->test){sys=s->test->syscalls;context=s->test->syscall_context;}
#endif
    if(ota_linux_mtd_open(&s->mtd,s->plan,&isolation,OTA_MTD_SYNC_RELEASE49,sys,context,&backend,s->error,s->error_size))return -1;
    int rc=ota_apply_plan_run(s->plan,&backend,&s->result->progress,s->error,s->error_size);
    if(rc){ota_linux_mtd_close(&s->mtd);return -1;}
    if(ota_linux_mtd_finish(&s->mtd,s->error,s->error_size))return -1;
    if(ota_apply_plan_guard(s->plan,s->error,s->error_size))return -1;
    s->result->verified_written=1;
    if(progress(s,"written_reboot_pending"))return fail(s->error,s->error_size,"verified writes complete but RAM status publication failed");
    return 0;
}
static void initialize(struct install *s,struct ota_apply_plan *plan,int report,
                       struct ram_install_result *result,char *error,size_t size)
{
    memset(s,0,sizeof *s);memset(result,0,sizeof *result);
    s->plan=plan;s->result=result;s->report_fd=report;s->error=error;s->error_size=size;
    s->mtd.fd[0]=s->mtd.fd[1]=s->mtd.sync_fd[0]=s->mtd.sync_fd[1]=-1;
    result->sequence=plan->verified.sequence;
}
int ram_install_apply_inherited(const struct ram_install_fds *fds,
                               struct ram_install_result *result,char *error,size_t size)
{
    if(!fds||!result||(!error&&size))return -1;
    memset(result,0,sizeof *result);if(size)error[0]=0;
    if(geteuid()!=0||private_dir(fds->state,0)||private_dir(fds->stage,1)||private_dir(fds->progress,1))
        return fail(error,size,"private trusted inherited descriptors required");
    struct stat key,report,state,stage;struct statfs fs;
    if(fstat(fds->key,&key)||!S_ISREG(key.st_mode)||key.st_uid!=0||(key.st_mode&077)||key.st_nlink!=1||key.st_size!=32||
       fstatfs(fds->key,&fs)||!memory_fs((unsigned long)fs.f_type)||
       (fcntl(fds->key,F_GETFL)&O_ACCMODE)!=O_RDONLY||
       fstat(fds->progress,&report)||fstat(fds->state,&state)||fstat(fds->stage,&stage)||
       (report.st_dev==state.st_dev&&report.st_ino==state.st_ino)||
       (report.st_dev==stage.st_dev&&report.st_ino==stage.st_ino))return fail(error,size,"private RAM key/progress identity mismatch");
    uint8_t public_key[32],current_boot[16];
    if(pread(fds->key,public_key,sizeof public_key,0)!=sizeof public_key||boot_id(current_boot))
        return fail(error,size,"trusted key or current boot identity unavailable");
    struct ota_apply_plan plan;
    if(ota_apply_plan_open_locked(&plan,fds->state,fds->stage,fds->lock,public_key,"/proc/mtd",error,size))return -1;
    struct install s;initialize(&s,&plan,fds->progress,result,error,size);
    int rc=execute(&s,current_boot);
    if(rc)progress(&s,"failed");
    ota_apply_plan_close(&plan);return rc;
}
int ram_install_exit_status(int outcome,const struct ram_install_result *result)
{
    if(!result)return 11;
    if(!outcome)return result->verified_written?0:11;
    return result->may_have_changed?11:10;
}
#ifdef CAMERA_RAM_INSTALL_TEST
int ram_install_test_cmdline(const char *text)
{
    char args[2048];if(!text||strlen(text)>=sizeof args)return -1;
    strcpy(args,text);return cmdline(args);
}
int ram_install_test_execute(struct ota_apply_plan *plan,int report,const uint8_t id[16],
                            const struct ram_install_test_ops *ops,
                            struct ram_install_result *result,char *error,size_t size)
{
    if(!plan||!ops||!ops->environment||!ops->syscalls||!result||!id||private_dir(report,1))
        return fail(error,size,"complete fake-only installer test required");
    struct install s;initialize(&s,plan,report,result,error,size);s.test=ops;
    int rc=execute(&s,id);if(rc)progress(&s,"failed");return rc;
}
#endif
