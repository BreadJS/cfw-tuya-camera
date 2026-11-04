#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "update.h"
#include "../../ota/vendor/monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#define RESERVE UINT64_C(4194304)
static void rejected(struct control_reply *r,unsigned status,const char *why)
{r->status=status;snprintf(r->json,sizeof r->json,"{\"error\":\"%s\",\"applied\":false,\"install_available\":false}",why);}
static int directory(const char *path,int ram)
{
    if(mkdir(path,0700)&&errno!=EEXIST)return -1;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    struct stat st;struct statfs fs;
    if(fstat(fd,&st)||st.st_uid!=0||(st.st_mode&077)||fstatfs(fd,&fs)){
        close(fd);return -1;
    }
    int memory=fs.f_type==0x01021994||fs.f_type==0x858458f6;
#ifdef CAMERA_RAM_BOOT_TEST
    /* Explicit volatile trial: exercise signatures/staging without NOR state.
     * This does not authorize installation or commit an installed release. */
    if(!memory){
#else
    if(ram?!memory:fs.f_type!=0x72b6){
#endif
        close(fd);return -1;
    }
#ifdef CAMERA_RAM_BOOT_TEST
    (void)ram;
#endif
    return fd;
}
static int absent(int fd,const char *name)
{struct stat st;return fstatat(fd,name,&st,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT;}
static void cache_clear(struct control_update *u)
{if(u->cached&&u->cache_fd>=0)close(u->cache_fd);u->cached=0;u->cache_fd=-1;}
static int private_capsule(const struct stat *s)
{return S_ISREG(s->st_mode)&&s->st_uid==0&&!(s->st_mode&077)&&s->st_nlink==1;}
static int same_file(const struct stat *a,const struct stat *b)
{
    return private_capsule(a)&&private_capsule(b)&&a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&
        a->st_size==b->st_size&&a->st_mode==b->st_mode&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int record_matches(int directory,const char *name,const uint8_t *expected,size_t bytes)
{
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 0;
    struct stat a,b;uint8_t data[OTA_HEADER_SIZE];int match=0;
    if(bytes>sizeof data||fstat(fd,&a)||!private_capsule(&a)||a.st_size!=(off_t)bytes)goto done;
    size_t done_bytes=0;
    while(done_bytes<bytes){ssize_t n=read(fd,data+done_bytes,bytes-done_bytes);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto done;
        done_bytes+=(size_t)n;}
    if(!fstat(fd,&b)&&same_file(&a,&b)&&!memcmp(data,expected,bytes))match=1;
done:
    close(fd);return match;
}
static void cache_store(struct control_update *u,int verified_fd)
{
    cache_clear(u);
    struct stat opened,named;
    if(!u->state.has_pending||fstat(verified_fd,&opened)||
       fstatat(u->state.staging,"pending.cap",&named,AT_SYMLINK_NOFOLLOW)||!same_file(&opened,&named))return;
    int retained=fcntl(verified_fd,F_DUPFD_CLOEXEC,3);if(retained<0)return;
    u->cache_fd=retained;u->cache_identity=opened;u->cache_accepted=u->state.accepted;
    memcpy(u->cache_header,u->state.pending.header,OTA_HEADER_SIZE);u->cached=1;
}
static int cache_unchanged(struct control_update *u)
{
    if(!u->cached)return 0;
    struct stat opened,named;
    if(fstat(u->cache_fd,&opened)||!same_file(&opened,&u->cache_identity)||
       fstatat(u->state.staging,"pending.cap",&named,AT_SYMLINK_NOFOLLOW)||!same_file(&opened,&named))return 0;
    char accepted[32];int n=snprintf(accepted,sizeof accepted,"%"PRIu64"\n",u->cache_accepted);
    if(n<=0||(size_t)n>=sizeof accepted||
       !record_matches(u->state.directory,"accepted",(const uint8_t*)accepted,(size_t)n)||
       !record_matches(u->state.directory,"pending",u->cache_header,OTA_HEADER_SIZE))return 0;
    return !fstat(u->cache_fd,&opened)&&same_file(&opened,&u->cache_identity);
}
int control_update_init(struct control_update *u,const char *key_path,const char *state_path,
                         const char *stage_path,int initialize_missing)
{
    if(!u||!key_path||!state_path||!stage_path||geteuid()!=0||
       (initialize_missing!=0&&initialize_missing!=1))return -1;
    memset(u,0,sizeof*u);u->state.directory=u->state.staging=u->state.lock=-1;
    u->cache_fd=-1;u->progress_directory=-1;
    uint8_t key[32];char error[256];
    if(ota_read_public_key(key_path,key,1,error,sizeof error))return -1;
    int state=directory(state_path,0),stage=directory(stage_path,1),lock=-1,fd=-1,rc=-1;
    if(state<0||stage<0)goto done;
    lock=openat(state,"lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    struct stat ls;
    if(lock<0||fstat(lock,&ls)||!S_ISREG(ls.st_mode)||ls.st_uid!=0||
       (ls.st_mode&077)||ls.st_nlink!=1||flock(lock,LOCK_EX|LOCK_NB))goto done;
    if(absent(state,"accepted")) {
        if(!initialize_missing||!absent(state,"pending")||!absent(stage,"pending.cap"))goto done;
        /* Exclusive create never resets an existing anti-replay baseline.
         * A torn first provision fails closed on the next open. */
        fd=openat(state,"accepted",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0||write(fd,"1\n",2)!=2||fsync(fd)||fsync(state))goto done;
        if(close(fd)){fd=-1;goto done;}fd=-1;
    }
    close(lock);lock=-1;
    if(ota_state_open(&u->state,state_path,stage_path,key,error,sizeof error))goto done;
    if(pthread_mutex_init(&u->mutex,NULL)){ota_state_close(&u->state);goto done;}
    u->initialized=1;rc=0;
done:
    if(fd>=0)close(fd);
    if(lock>=0)close(lock);
    if(state>=0)close(state);
    if(stage>=0)close(stage);
    return rc;
}
int control_update_close(struct control_update *u)
{
    if(!u||!u->initialized)return 0;
    pthread_mutex_lock(&u->mutex);
    if(u->monitor_active||u->install_pid>0){pthread_mutex_unlock(&u->mutex);return -EBUSY;}
    cache_clear(u);ota_state_close(&u->state);
    if(u->progress_directory>=0)close(u->progress_directory);
    u->initialized=0;pthread_mutex_unlock(&u->mutex);pthread_mutex_destroy(&u->mutex);return 0;
}
static uint64_t available(void)
{
    FILE *f=fopen("/proc/meminfo","r");if(!f)return 0;
    char line[256];unsigned long long kb;uint64_t result=0;
    while(fgets(line,sizeof line,f))if(sscanf(line,"MemAvailable: %llu kB",&kb)==1&&kb<=UINT64_MAX/1024){result=(uint64_t)kb*1024;break;}
    fclose(f);return result;
}
static int exact(const struct control_body *b,uint8_t *out,size_t n)
{
    while(n){ssize_t got=b->read(b->opaque,out,n);if(got<0&&errno==EINTR)continue;
        if(got<=0||(size_t)got>n)return -1;
        out+=got;n-=(size_t)got;}
    return 0;
}
static int written(int fd,const uint8_t *p,size_t n)
{
    while(n){ssize_t got=write(fd,p,n);if(got<0&&errno==EINTR)continue;
        if(got<=0)return -1;
        p+=got;n-=(size_t)got;}
    return 0;
}
void control_update_get_status(struct control_update *u,struct control_reply *r)
{
    if(!r)return;
    if(!u||!u->initialized){rejected(r,503,"update_unavailable");return;}
    if(pthread_mutex_trylock(&u->mutex)){rejected(r,409,"update_busy");return;}
    char error[256];int staged=-1;
    int present=cache_unchanged(u);
    if(!present){
        cache_clear(u);
        if(ota_state_refresh(&u->state,error,sizeof error)){rejected(r,503,"state_unavailable");goto done;}
        present=ota_state_staged(&u->state,&staged,error,sizeof error);
        if(present<0){rejected(r,503,"staged_capsule_invalid");goto done;}
        if(present)cache_store(u,staged);
    }
    char digest[129]="";static const char hex[]="0123456789abcdef";
    if(u->state.has_pending)for(unsigned i=0;i<64;i++){unsigned b=u->state.pending.header[64+i];digest[2*i]=hex[b>>4];digest[2*i+1]=hex[b&15];}
    r->status=200;
    snprintf(r->json,sizeof r->json,"{\"accepted\":%"PRIu64",\"pending\":%"PRIu64
        ",\"verified_pending\":%s,\"signed_pending_record\":%s,\"staged\":%s,\"payload_digest\":\"%s\",\"applied\":false,\"install_available\":%s}",
        u->state.accepted,u->state.has_pending?u->state.pending.sequence:0,
        present&&u->state.has_pending?"true":"false",u->state.has_pending?"true":"false",present?"true":"false",digest,control_update_can_install_locked(u)?"true":"false");
    control_update_install_status_locked(u,r->json,sizeof r->json);
done:
    if(staged>=0)close(staged);
    pthread_mutex_unlock(&u->mutex);
}
void control_update_cancel(struct control_update *u,struct control_reply *r)
{
    if(!r)return;
    if(!u||!u->initialized){rejected(r,503,"update_unavailable");return;}
    if(pthread_mutex_trylock(&u->mutex)){rejected(r,409,"update_busy");return;}
    cache_clear(u);
    char error[256];
    if(ota_state_refresh(&u->state,error,sizeof error)){
        rejected(r,503,"state_unavailable");goto done;
    }
    if(control_update_discard_install_locked(u)){
        rejected(r,409,"installation_started_retry_required");goto done;
    }
    /* Remove RAM bytes first, durably, before permitting another signed
     * pending identity. A partial failure keeps accepted unchanged and does
     * not claim successful cancellation. Missing RAM after reboot is normal. */
    if((unlinkat(u->state.staging,"pending.cap",0)&&errno!=ENOENT)||fsync(u->state.staging)){
        rejected(r,503,"discard_stage_incomplete");goto done;
    }
    if((unlinkat(u->state.directory,"pending",0)&&errno!=ENOENT)||fsync(u->state.directory)){
        rejected(r,503,"discard_journal_incomplete");goto done;
    }
    if(ota_state_refresh(&u->state,error,sizeof error)){
        rejected(r,503,"discard_refresh_failed");goto done;
    }
    r->status=200;
    u->install_pristine=u->install_failed=0;u->install_sequence=0;memset(u->install_header,0,sizeof u->install_header);
    snprintf(r->json,sizeof r->json,"{\"discarded\":true,\"accepted\":%"PRIu64
        ",\"pending\":0,\"verified_pending\":false,\"staged\":false,\"applied\":false,\"install_available\":false}",u->state.accepted);
done:
    pthread_mutex_unlock(&u->mutex);
}
void control_update_upload(struct control_update *u,const struct control_body *body,
                            struct control_reply *r)
{
    if(!r)return;
    if(!u||!u->initialized){rejected(r,503,"update_unavailable");return;}
    if(!body||!body->read||body->length<OTA_HEADER_SIZE+32){rejected(r,400,"invalid_length");return;}
    if(body->length>OTA_CAPSULE_LIMIT){rejected(r,413,"capsule_too_large");return;}
    if(pthread_mutex_trylock(&u->mutex)){rejected(r,409,"update_busy");return;}
    if(u->reset_pending||u->monitor_active||u->install_pid>0||u->install_written){rejected(r,409,"installation_in_progress");pthread_mutex_unlock(&u->mutex);return;}
    cache_clear(u);
    uint8_t header[OTA_HEADER_SIZE],buffer[4096],digest[64];char error[256],name[96]={0};
    int fd=-1,existing=-1,retry=0;struct ota_verified incoming,verified;
    if(exact(body,header,sizeof header)){rejected(r,400,"truncated_capsule");goto done;}
    if(ota_verify_header(header,u->state.key,0,&incoming,error,sizeof error)){rejected(r,403,"authentication_failed");goto done;}
    if(incoming.payload_size!=body->length-OTA_HEADER_SIZE){rejected(r,400,"length_mismatch");goto done;}
    if(ota_state_refresh(&u->state,error,sizeof error)){rejected(r,503,"state_unavailable");goto done;}
    if(incoming.sequence<=u->state.accepted||(u->state.has_pending&&
        memcmp(header,u->state.pending.header,sizeof header))){rejected(r,409,"sequence_or_pending_conflict");goto done;}
    if(u->state.has_pending){int p=ota_state_staged(&u->state,&existing,error,sizeof error);
        if(p<0){rejected(r,503,"staged_capsule_invalid");goto done;}retry=p==1;}
    if(!retry){uint64_t memory=available();
        if(memory<RESERVE||body->length>memory-RESERVE){rejected(r,507,"memory_budget");goto done;}
        fd=ota_state_upload(&u->state,name,sizeof name,error,sizeof error);
        if(fd<0||written(fd,header,sizeof header)){rejected(r,507,"staging_write_failed");goto done;}
    }
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,sizeof digest);
    uint64_t remaining=incoming.payload_size;
    while(remaining){size_t n=remaining>sizeof buffer?sizeof buffer:(size_t)remaining;
        if(exact(body,buffer,n)){rejected(r,400,"truncated_capsule");goto done;}
        crypto_blake2b_update(&hash,buffer,n);
        if(fd>=0&&written(fd,buffer,n)){rejected(r,507,"staging_write_failed");goto done;}
        remaining-=n;
    }
    crypto_blake2b_final(&hash,digest);
    if(crypto_verify64(digest,header+64)){rejected(r,403,"payload_digest_failed");goto done;}
    if(!retry){int resumed;
        if(ota_verify_fd(fd,u->state.key,u->state.accepted,&verified,error,sizeof error)){rejected(r,422,"image_validation_failed");goto done;}
        if(ota_state_publish(&u->state,fd,name,&verified,&resumed,error,sizeof error)){rejected(r,503,"journal_commit_failed");goto done;}
        name[0]=0;
    }
    cache_store(u,retry?existing:fd);
    char hex[129];static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<64;i++){hex[2*i]=digits[digest[i]>>4];hex[2*i+1]=digits[digest[i]&15];}hex[128]=0;
    r->status=retry?200:201;
    snprintf(r->json,sizeof r->json,"{\"staged\":true,\"verified_pending\":true,\"already_pending\":%s,"
        "\"sequence\":%"PRIu64",\"payload_digest\":\"%s\",\"applied\":false,\"install_available\":false}",
        retry?"true":"false",incoming.sequence,hex);
done:
    if(fd>=0)close(fd);
    if(existing>=0)close(existing);
    if(*name)unlinkat(u->state.staging,name,0);
    pthread_mutex_unlock(&u->mutex);
}
