#define _GNU_SOURCE
#include "update.h"
#include "../../ota/ota_apply_plan.h"
#include "../../ota/boot_confirm.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef CAMERA_UPDATE_INSTALL_TEST
#include "../../../tests/update_bridge/hooks.h"
#define INSTALLER camera_test_installer()
#define PROGRESS camera_test_progress()
#define MTD_MAP camera_test_mtd_map()
#define fstat camera_test_fstat
#define fstatat camera_test_fstatat
#define fstatfs camera_test_fstatfs
#define fork camera_test_fork
#define pthread_create camera_test_pthread_create
#define sync camera_test_sync
#define reboot camera_test_reboot
#else
#define INSTALLER "/usr/libexec/camera/firmware-install"
#define PROGRESS "/var/tmp/camera-update-progress"
#define MTD_MAP "/proc/mtd"
#endif
static void reject(struct control_reply *r,unsigned code,const char *why)
{r->status=code;snprintf(r->json,sizeof r->json,"{\"error\":\"%s\",\"installation_started\":false}",why);}
static int memory(int fd)
{struct statfs fs;return !fstatfs(fd,&fs)&&((unsigned long)fs.f_type==0x01021994UL||(unsigned long)fs.f_type==0x858458f6UL);}
static int owned(int fd,off_t bytes)
{struct stat s;return !fstat(fd,&s)&&S_ISREG(s.st_mode)&&s.st_uid==0&&!(s.st_mode&077)&&s.st_nlink==1&&s.st_size==bytes;}
int control_update_can_install_locked(struct control_update *u)
{
#if defined(CAMERA_UPDATE_INSTALL_TEST) && !defined(CAMERA_RAM_BOOT_TEST)
    return u&&u->initialized&&!u->reset_pending&&!u->monitor_active&&u->install_pid<=0&&!u->install_written;
#elif defined(CAMERA_RAM_BOOT_TEST)
    (void)u;return 0;
#else
    if(!u||!u->initialized||u->reset_pending||u->monitor_active||u->install_pid>0||u->install_written)return 0;
    struct statfs fs;if(fstatfs(u->state.directory,&fs)||(unsigned long)fs.f_type!=0x72b6UL)return 0;
    int fd=open("/.camera-boot-origin",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;
    const char expected[]="NOR_ROOT_V1\n";char text[sizeof expected];
    struct stat s;int good=owned(fd,sizeof expected-1)&&!fstat(fd,&s)&&(s.st_mode&0777)==0400&&memory(fd)&&
        pread(fd,text,sizeof expected,0)==(ssize_t)sizeof expected-1&&!memcmp(text,expected,sizeof expected-1);
    close(fd);if(!good)return 0;
    fd=open(INSTALLER,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 0;
    good=!fstat(fd,&s)&&S_ISREG(s.st_mode)&&s.st_uid==0&&!(s.st_mode&022)&&(s.st_mode&0111)&&s.st_nlink==1&&memory(fd);
    close(fd);return good;
#endif
}
int control_update_busy(struct control_update *u)
{
    if(!u||!u->initialized)return 0;
    pthread_mutex_lock(&u->mutex);int busy=u->reset_pending||u->monitor_active||u->install_pid>0||u->install_written;
    pthread_mutex_unlock(&u->mutex);return busy;
}
void control_update_install_status_locked(struct control_update *u,char *json,size_t cap)
{
    size_t n=strlen(json);if(!n||json[n-1]!='}')return;
    const char *phase=u->install_written?"written_reboot_pending":u->install_pid>0?"installing":u->install_failed?"failed":"idle";
    char progress[1025]="";
    if(u->progress_directory>=0){
        int fd=openat(u->progress_directory,"install-progress.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd>=0){struct stat st;
            if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&!(st.st_mode&077)&&st.st_nlink==1&&st.st_size>1&&st.st_size<=1024){
                ssize_t bytes=pread(fd,progress,(size_t)st.st_size,0);
                if(bytes==st.st_size&&!memchr(progress,0,(size_t)bytes))progress[bytes]=0;
                else progress[0]=0;
            }close(fd);
        }
    }
    char quoted[sizeof progress*6+3];if(control_json_string(quoted,sizeof quoted,progress))strcpy(quoted,"\"\"");
    struct stat journal;int absent=fstatat(u->state.directory,"install-pending",&journal,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT;
    int discard=!u->reset_pending&&!u->monitor_active&&!u->install_written&&(absent||u->install_pristine);
    snprintf(json+n-1,cap-n+1,",\"installation_phase\":\"%s\",\"installation_sequence\":%llu,\"installation_progress\":%s,\"discard_available\":%s}",phase,(unsigned long long)u->install_sequence,quoted,discard?"true":"false");
}
int control_update_discard_install_locked(struct control_update *u)
{
    if(u->reset_pending||u->monitor_active||u->install_pid>0||u->install_written)return -1;
    int fd=openat(u->state.directory,"install-pending",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?0:-1;
    unsigned char record[OTA_BOOT_JOURNAL_BYTES];struct stat before,after;
    int safe=u->install_pristine&&u->state.has_pending&&owned(fd,sizeof record)&&
        !fstat(fd,&before)&&pread(fd,record,sizeof record,0)==sizeof record&&!fstat(fd,&after)&&
        before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&
        before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec&&
        !memcmp(record+OTA_BOOT_JOURNAL_HEADER_OFFSET,u->install_header,OTA_HEADER_SIZE)&&!memcmp(record+OTA_BOOT_JOURNAL_HEADER_OFFSET,u->state.pending.header,OTA_HEADER_SIZE);
    close(fd);if(!safe)return -1;
    return unlinkat(u->state.directory,"install-pending",0)||fsync(u->state.directory)?-1:0;
}
void control_update_plan(struct control_update *u,struct control_reply *r)
{
    if(!u||!u->initialized){reject(r,503,"update_unavailable");return;}
    if(pthread_mutex_trylock(&u->mutex)){reject(r,409,"update_busy");return;}
    struct ota_apply_plan *plan=NULL;char error[128];
    if(u->reset_pending||u->monitor_active||u->install_pid>0||u->install_written){reject(r,409,"installation_in_progress");goto done;}
    plan=calloc(1,sizeof *plan);if(!plan){reject(r,507,"memory_budget");goto done;}
    if(ota_apply_plan_open_locked(plan,u->state.directory,u->state.staging,u->state.lock,u->state.key,MTD_MAP,error,sizeof error)){
        reject(r,409,"verified_pending_plan_unavailable");goto done;
    }
    uint64_t written=0,erased=0;unsigned selected=0;
    for(unsigned i=0;i<plan->block_count;i++){written+=plan->blocks[i].data_bytes;erased+=plan->blocks[i].erase_bytes;selected|=1u<<plan->blocks[i].mtd_index;}
    r->status=200;snprintf(r->json,sizeof r->json,"{\"read_only\":true,\"installation_started\":false,\"sequence\":%llu,\"kernel_selected\":%s,\"rootfs_selected\":%s,\"blocks\":%u,\"write_bytes\":%llu,\"erase_bytes\":%llu,\"readback_bytes\":%llu,\"protected_mtd\":[0,1,2,5],\"install_available\":%s}",
        (unsigned long long)plan->verified.sequence,selected&(1u<<3)?"true":"false",selected&(1u<<4)?"true":"false",plan->block_count,(unsigned long long)written,(unsigned long long)erased,(unsigned long long)erased,control_update_can_install_locked(u)?"true":"false");
    ota_apply_plan_close(plan);
done:
    free(plan);pthread_mutex_unlock(&u->mutex);
}
static int progress_directory(void)
{
    if(mkdir(PROGRESS,0700)&&errno!=EEXIST)return -1;
    int fd=open(PROGRESS,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat s;
    if(fd<0)return -1;
    if(fstat(fd,&s)||s.st_uid!=0||(s.st_mode&077)||!memory(fd)){close(fd);return -1;}
    return fd;
}
static int public_copy(struct control_update *u,int directory)
{
    int fd=openat(directory,"install-key",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0&&errno==ENOENT){
        fd=openat(directory,"install-key",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0)return -1;
        int rc=write(fd,u->state.key,32)==32&&!fsync(fd)?0:-1;
        if(close(fd))rc=-1;
        if(rc)return -1;
        fd=openat(directory,"install-key",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    }
    if(fd<0)return -1;
    uint8_t key[33];if(!owned(fd,32)||!memory(fd)||pread(fd,key,sizeof key,0)!=32||memcmp(key,u->state.key,32)){
        close(fd);return -1;
    }return fd;
}
static int descriptor_bound(unsigned *bound)
{
    struct rlimit limit;if(getrlimit(RLIMIT_NOFILE,&limit)||limit.rlim_cur>65536)return -1;
    unsigned highest=(unsigned)limit.rlim_cur;DIR *fds=opendir("/proc/self/fd");if(!fds)return -1;
    struct dirent *entry;int rc=0;
    while((entry=readdir(fds))){
        if(entry->d_name[0]<'0'||entry->d_name[0]>'9')continue;
        char *end;errno=0;unsigned long number=strtoul(entry->d_name,&end,10);
        if(errno||*end||number>=65536){rc=-1;break;}
        if(number+1>highest)highest=(unsigned)number+1;
    }
    closedir(fds);*bound=highest;return rc;
}
static void *monitor(void *opaque)
{
    struct control_update *u=opaque;int status=0;pid_t pid;
    pthread_mutex_lock(&u->mutex);pid=u->install_pid;pthread_mutex_unlock(&u->mutex);
    pid_t result;do{result=waitpid(pid,&status,0);}while(result<0&&errno==EINTR);
    int success=result==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
    int pristine_failure=result==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==10;
    pthread_mutex_lock(&u->mutex);u->install_pid=0;u->install_failed=!success;u->install_written=success;
    if(!pristine_failure)u->install_pristine=0;
    if(!success)u->monitor_active=0;
    pthread_mutex_unlock(&u->mutex);
    if(success){
        struct timespec grace={.tv_sec=2};while(nanosleep(&grace,&grace)&&errno==EINTR){}
        sync();if(reboot(RB_AUTOBOOT)){
            pthread_mutex_lock(&u->mutex);u->install_failed=1;u->monitor_active=0;pthread_mutex_unlock(&u->mutex);
        }
    }return NULL;
}
void control_update_install(struct control_update *u,uint64_t expected_sequence,
                            const char *expected_digest,struct control_reply *r)
{
    if(!u||!u->initialized){reject(r,503,"update_unavailable");return;}
    if(pthread_mutex_trylock(&u->mutex)){reject(r,409,"update_busy");return;}
    int progress=-1,key=-1,log=-1,nullfd=-1,gate[2]={-1,-1},keep[8];for(unsigned i=0;i<8;i++)keep[i]=-1;
    struct ota_apply_plan *plan=NULL;char error[128];
    if(!control_update_can_install_locked(u)){reject(r,409,"installation_requires_installed_RAM_firmware");goto done;}
    plan=calloc(1,sizeof *plan);if(!plan){reject(r,507,"memory_budget");goto done;}
    if(ota_apply_plan_open_locked(plan,u->state.directory,u->state.staging,u->state.lock,u->state.key,MTD_MAP,error,sizeof error)){
        reject(r,409,"verified_pending_plan_unavailable");goto done;
    }
    char digest[129];static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<64;i++){unsigned byte=plan->verified.header[64+i];digest[2*i]=hex[byte>>4];digest[2*i+1]=hex[byte&15];}digest[128]=0;
    if(!expected_digest||strlen(expected_digest)!=128||expected_sequence!=plan->verified.sequence||strcmp(expected_digest,digest)){
        ota_apply_plan_close(plan);reject(r,409,"staged_update_changed_review_again");goto done;
    }
    struct stat prior;int has_journal=!fstatat(u->state.directory,"install-pending",&prior,AT_SYMLINK_NOFOLLOW);
    if(!has_journal&&errno!=ENOENT){reject(r,503,"installation_journal_unavailable");ota_apply_plan_close(plan);goto done;}
    u->install_pristine=!has_journal||(u->install_pristine&&!memcmp(u->install_header,plan->verified.header,OTA_HEADER_SIZE));
    memcpy(u->install_header,plan->verified.header,OTA_HEADER_SIZE);
    u->install_sequence=plan->verified.sequence;ota_apply_plan_close(plan);
    progress=progress_directory();if(progress<0){reject(r,503,"private_progress_unavailable");goto done;}
    key=public_copy(u,progress);if(key<0){reject(r,503,"retained_trust_anchor_unavailable");goto done;}
    log=openat(progress,"install.log",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(log<0&&errno==EEXIST){struct stat st;
        if(!fstatat(progress,"install.log",&st,AT_SYMLINK_NOFOLLOW)&&S_ISREG(st.st_mode)&&st.st_uid==0&&!(st.st_mode&077)&&st.st_nlink==1&&unlinkat(progress,"install.log",0)==0)
            log=openat(progress,"install.log",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    }
    nullfd=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if(log<0||nullfd<0||pipe2(gate,O_CLOEXEC)){reject(r,503,"installer_handoff_unavailable");goto done;}
    int fds[8]={u->state.directory,u->state.staging,u->state.lock,key,progress,log,nullfd,gate[0]};
    for(unsigned i=0;i<8;i++)if((keep[i]=fcntl(fds[i],F_DUPFD_CLOEXEC,64))<0){reject(r,503,"installer_descriptor_handoff_failed");goto done;}
    unsigned close_bound;if(descriptor_bound(&close_bound)){reject(r,503,"descriptor_limit_unavailable");goto done;}
    pid_t child=fork();
    if(child==0){
        /* After multithreaded fork: async-signal-safe syscalls only until exec.
         * The gate prevents any apply if monitor creation fails in parent. */
        close(gate[1]);
        unsigned char start;ssize_t n;do{n=read(keep[7],&start,1);}while(n<0&&errno==EINTR);
        if(n!=1||start!=0xa5)_exit(125);
        for(unsigned i=0;i<5;i++)if(dup2(keep[i],3+(int)i)<0)_exit(126);
        if(dup2(keep[6],0)<0||dup2(keep[5],1)<0||dup2(keep[5],2)<0)_exit(126);
        for(int fd=8;fd<(int)close_bound;fd++)close(fd);
        char *const arguments[]={INSTALLER,"--apply-inherited",NULL};char *const environment[]={"PATH=/bin:/sbin:/usr/bin:/usr/sbin","TZ=UTC",NULL};
        execve(INSTALLER,arguments,environment);_exit(127);
    }
    if(child<0){reject(r,503,"installer_process_unavailable");goto done;}
    u->install_pid=child;u->install_failed=u->install_written=0;u->monitor_active=1;
    if(u->progress_directory>=0)close(u->progress_directory);
    u->progress_directory=progress;progress=-1;
    pthread_t thread;int rc=pthread_create(&thread,NULL,monitor,u);
    if(rc){
        close(gate[1]);gate[1]=-1;int status;while(waitpid(child,&status,0)<0&&errno==EINTR){}
        u->install_pid=0;u->monitor_active=0;reject(r,503,"installer_monitor_unavailable");goto done;
    }
    pthread_detach(thread);
    unsigned char start=0xa5;if(write(gate[1],&start,1)!=1){reject(r,503,"installer_start_gate_failed");goto done;}
    r->status=202;snprintf(r->json,sizeof r->json,"{\"installation_started\":true,\"sequence\":%llu,\"reboot_after_verified_write\":true}",(unsigned long long)u->install_sequence);
done:
    for(unsigned i=0;i<8;i++)if(keep[i]>=0)close(keep[i]);
    if(gate[0]>=0)close(gate[0]);
    if(gate[1]>=0)close(gate[1]);
    if(log>=0)close(log);
    if(nullfd>=0)close(nullfd);
    if(key>=0)close(key);
    if(progress>=0)close(progress);
    free(plan);pthread_mutex_unlock(&u->mutex);
}
