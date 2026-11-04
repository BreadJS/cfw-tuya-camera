#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "boot_confirm.h"
#include "vendor/monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t magic[8]={'F','H','C','B','O','O','T','1'};
static const uint8_t vendor[32]={'F','u','h','a','n','8','6','2','6','V','1','0','0',0,0,0,0,0,0,0,0xab,0xcd,0x12,0x34,0,0,0,0,0,0,0,0};
static unsigned long serial;
static int fail(char *e,size_t n,const char *s){if(n)snprintf(e,n,"%s",s);return -1;}
static uint32_t le(const uint8_t *p){return(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t be(const uint8_t *p){return(uint32_t)p[3]|(uint32_t)p[2]<<8|(uint32_t)p[1]<<16|(uint32_t)p[0]<<24;}
static uint64_t le64(const uint8_t *p){return le(p)|(uint64_t)le(p+4)<<32;}
static void put(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void putbe(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[3-i]=(uint8_t)(v>>(i*8));}
static int nonzero(const uint8_t *p,size_t n){uint8_t v=0;while(n--)v|=*p++;return v!=0;}
static int safe_file(const struct stat *s)
{return S_ISREG(s->st_mode)&&s->st_uid==geteuid()&&!(s->st_mode&077)&&s->st_nlink==1;}
static int identity(const struct stat *a,const struct stat *b)
{return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_nlink==b->st_nlink&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;}
static int state_lock(int dir,int lock)
{
    struct stat d,a,b;
    return fstat(dir,&d)||!S_ISDIR(d.st_mode)||d.st_uid!=geteuid()||(d.st_mode&077)||
        fstat(lock,&a)||!safe_file(&a)||fstatat(dir,"lock",&b,AT_SYMLINK_NOFOLLOW)||
        !safe_file(&b)||!identity(&a,&b)||flock(lock,LOCK_EX|LOCK_NB);
}
/*1 means absent; no unbounded file reads or symlink/hardlink traversal. */
static int record(int dir,const char *name,uint8_t *out,size_t capacity,size_t *length)
{
    int fd=openat(dir,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return errno==ENOENT?1:-1;
    struct stat a,b;int rc=-1;
    if(fstat(fd,&a)||!safe_file(&a)||a.st_size<0||(uint64_t)a.st_size>capacity)goto done;
    size_t used=0;while(used<(size_t)a.st_size){ssize_t n=read(fd,out+used,(size_t)a.st_size-used);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;used+=(size_t)n;}
    uint8_t extra;ssize_t n;do{n=read(fd,&extra,1);}while(n<0&&errno==EINTR);
    if(n||fstat(fd,&b)||!identity(&a,&b))goto done;
    *length=used;rc=0;
done:close(fd);return rc;
}
static int accepted(int dir,uint64_t *value)
{
    uint8_t text[22];size_t length;
    if(record(dir,"accepted",text,21,&length)||length<2||text[length-1]!='\n')return -1;
    text[length-1]=0;return ota_parse_u64((const char *)text,value);
}
static int same_record(int dir,const char *name,const uint8_t *want,size_t length,int absent_ok)
{
    uint8_t data[OTA_BOOT_JOURNAL_BYTES];size_t got;int rc=record(dir,name,data,sizeof data,&got);
    if(rc==1)return absent_ok?0:-1;
    return rc||got!=length||memcmp(data,want,length)?-1:0;
}
static int exact_at(int fd,void *data,size_t size,uint64_t offset)
{
    uint8_t *p=data;while(size){ssize_t n=pread(fd,p,size,(off_t)offset);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;offset+=(uint64_t)n;}return 0;
}
static int injected(ota_boot_fault f,void *c,const char *point){return f&&f(c,point);}
static int temporary_name(const char *name)
{
    const char *p=name;if(strncmp(p,"boot-confirm-",13))return 0;p+=13;
    if(*p<'0'||*p>'9')return 0;
    while(*p>='0'&&*p<='9')p++;
    if(*p++!='-'||*p<'0'||*p>'9')return 0;
    while(*p>='0'&&*p<='9')p++;
    return !strcmp(p,".tmp");
}
static int cleanup_temporaries(int dir)
{
    int copy=openat(dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(copy<0)return -1;
    DIR *entries=fdopendir(copy);if(!entries){close(copy);return -1;}
    int bad=0,removed=0;struct dirent *entry;
    while((entry=readdir(entries))){
        if(!temporary_name(entry->d_name))continue;
        struct stat st;
        if(fstatat(dir,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)||!safe_file(&st)||unlinkat(dir,entry->d_name,0)){bad=1;break;}
        removed=1;
    }
    closedir(entries);return bad||(removed&&fsync(dir))?-1:0;
}
static int atomic(int dir,const char *name,const uint8_t *data,size_t size,
                  const char *prefix,ota_boot_fault hook,void *context)
{
    char temporary[96];int fd=-1;
    for(unsigned attempt=0;attempt<100;attempt++){
        int n=snprintf(temporary,sizeof temporary,"boot-confirm-%ld-%lu.tmp",(long)getpid(),++serial);
        if(n<0||(size_t)n>=sizeof temporary)return -1;
        fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd>=0)break;
        if(errno!=EEXIST)return -1;
    }
    if(fd<0)return -1;
    int bad=0;size_t done=0;while(done<size){ssize_t n=write(fd,data+done,size-done);if(n<0&&errno==EINTR)continue;if(n<=0){bad=1;break;}done+=(size_t)n;}
    if(!bad&&fsync(fd))bad=1;
    if(close(fd))bad=1;
    char point[64];snprintf(point,sizeof point,"%s_file_synced",prefix);
    if(!bad&&injected(hook,context,point))bad=1;
    if(bad){unlinkat(dir,temporary,0);return -1;}
    if(renameat(dir,temporary,dir,name)){unlinkat(dir,temporary,0);return -1;}
    snprintf(point,sizeof point,"%s_renamed",prefix);if(injected(hook,context,point))return -1;
    if(fsync(dir))return -1;
    snprintf(point,sizeof point,"%s_directory_synced",prefix);return injected(hook,context,point)?-1:0;
}
static int journal_valid(const uint8_t j[OTA_BOOT_JOURNAL_BYTES],const uint8_t key[32],struct ota_verified *v)
{
    char error[128];unsigned count=le(j+12);
    if(memcmp(j,magic,8)||le(j+8)!=1||!count||count>2||!nonzero(j+16,16)||ota_verify_header(j+OTA_BOOT_JOURNAL_HEADER_OFFSET,key,0,v,error,sizeof error))return -1;
    uint64_t size=32;uint32_t previous=0;
    for(unsigned i=0;i<count;i++){
        uint32_t kind=le(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+i*OTA_BOOT_JOURNAL_RECORD_BYTES),bytes=le(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+4+i*OTA_BOOT_JOURNAL_RECORD_BYTES);
        if((kind!=2&&kind!=3)||kind<=previous||bytes<(kind==2?64u:96u)||bytes>(kind==2?OTA_KERNEL_LIMIT:OTA_ROOTFS_LIMIT))return -1;
        size+=12u+bytes;previous=kind;
    }
    if(count==1&&nonzero(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+OTA_BOOT_JOURNAL_RECORD_BYTES,OTA_BOOT_JOURNAL_RECORD_BYTES))return -1;
    return size==v->payload_size?0:-1;
}
int ota_boot_prepare(struct ota_apply_plan *plan,const uint8_t id[16],ota_boot_fault hook,void *context,char *error,size_t size)
{
    if(!plan||!id||!nonzero(id,16)||state_lock(plan->state_dir,plan->lock_fd)||cleanup_temporaries(plan->state_dir)||ota_apply_plan_guard(plan,error,size))return fail(error,size,"unsafe install journal context");
    struct ota_verified verified;if(ota_verify_fd(plan->capsule_fd,plan->key,plan->accepted,&verified,error,size)||memcmp(verified.header,plan->verified.header,OTA_HEADER_SIZE))return fail(error,size,"install capsule no longer authentic");
    uint8_t j[OTA_BOOT_JOURNAL_BYTES]={0};memcpy(j,magic,8);put(j+8,1);put(j+12,verified.records);memcpy(j+16,id,16);memcpy(j+OTA_BOOT_JOURNAL_HEADER_OFFSET,verified.header,OTA_HEADER_SIZE);
    uint64_t position=OTA_HEADER_SIZE+32;for(unsigned i=0;i<verified.records;i++){
        uint8_t rec[12];if(i>=2||exact_at(plan->capsule_fd,rec,sizeof rec,position))return fail(error,size,"invalid install record map");
        put(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+i*OTA_BOOT_JOURNAL_RECORD_BYTES,be(rec));put(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+4+i*OTA_BOOT_JOURNAL_RECORD_BYTES,be(rec+4));position+=12u+be(rec+4);
    }
    struct ota_verified check;if(journal_valid(j,plan->key,&check)||ota_apply_plan_guard(plan,error,size))return fail(error,size,"invalid authenticated install journal");
    uint8_t old[OTA_BOOT_JOURNAL_BYTES];size_t length;int rc=record(plan->state_dir,OTA_BOOT_JOURNAL_NAME,old,sizeof old,&length);
    if(rc<0||(rc==0&&(length!=sizeof old||journal_valid(old,plan->key,&check)||memcmp(old+OTA_BOOT_JOURNAL_HEADER_OFFSET,j+OTA_BOOT_JOURNAL_HEADER_OFFSET,sizeof old-OTA_BOOT_JOURNAL_HEADER_OFFSET))))return fail(error,size,"conflicting install journal");
    if(atomic(plan->state_dir,OTA_BOOT_JOURNAL_NAME,j,sizeof j,"journal",hook,context))return fail(error,size,"install journal durability failed");
    return ota_apply_plan_guard(plan,error,size);
}
static uint32_t crc(uint32_t value,const uint8_t *p,size_t n)
{while(n--){value^=*p++;for(unsigned b=0;b<8;b++)value=(value>>1)^(0xedb88320u&(0u-(value&1)));}return value;}
static int installed_exact(const struct ota_boot_reader *r,unsigned mtd,uint32_t offset,void *data,size_t size)
{
    uint8_t *p=data;while(size){ssize_t n=r->read(r->context,mtd,offset,p,size);if(n<0&&errno==EINTR)continue;if(n<=0||(size_t)n>size)return -1;p+=n;offset+=(uint32_t)n;size-=(size_t)n;}return 0;
}
static int installed(const uint8_t j[OTA_BOOT_JOURNAL_BYTES],const struct ota_boot_reader *reader)
{
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,64);crypto_blake2b_update(&hash,vendor,sizeof vendor);
    for(unsigned i=0;i<le(j+12);i++){
        uint32_t kind=le(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+i*OTA_BOOT_JOURNAL_RECORD_BYTES),length=le(j+OTA_BOOT_JOURNAL_RECORDS_OFFSET+4+i*OTA_BOOT_JOURNAL_RECORD_BYTES);unsigned mtd=kind+1;
        uint8_t rec[12]={0},header[96],buffer[4096];putbe(rec,kind);putbe(rec+4,length);crypto_blake2b_update(&hash,rec,sizeof rec);
        size_t first=kind==2?64:96;if(installed_exact(reader,mtd,0,header,first))return -1;
        crypto_blake2b_update(&hash,header,first);uint32_t expected=0,checksum=UINT32_MAX;
        if(kind==2){
            uint8_t copy[64];memcpy(copy,header,sizeof copy);uint32_t hcrc=be(copy+4);memset(copy+4,0,4);
            if(be(copy)!=0x27051956u||~crc(UINT32_MAX,copy,sizeof copy)!=hcrc||be(copy+12)!=length-64||be(copy+16)!=0xa0008000u||be(copy+20)!=0xa0008000u||copy[28]!=5||copy[29]!=2||copy[30]!=2||copy[31])return -1;
            expected=be(copy+24);
        }else if(memcmp(header,"hsqs",4)||header[20]!=4||header[21]||header[28]!=4||header[29]||header[30]||header[31]||le64(header+40)!=length)return -1;
        uint32_t offset=(uint32_t)first;
        while(offset<length){size_t n=length-offset;if(n>sizeof buffer)n=sizeof buffer;if(installed_exact(reader,mtd,offset,buffer,n))return -1;crypto_blake2b_update(&hash,buffer,n);if(kind==2)checksum=crc(checksum,buffer,n);offset+=(uint32_t)n;}
        if(kind==2&&~checksum!=expected)return -1;
        /* The executor plans every erase block, including an FF tail. A boot
         * after power loss must not accept a merely complete image prefix. */
        uint32_t limit=kind==2?OTA_KERNEL_LIMIT:OTA_ROOTFS_LIMIT;
        while(offset<limit){size_t n=limit-offset;if(n>sizeof buffer)n=sizeof buffer;if(installed_exact(reader,mtd,offset,buffer,n))return -1;for(size_t p=0;p<n;p++)if(buffer[p]!=0xff)return -1;offset+=(uint32_t)n;}
    }
    uint8_t digest[64];crypto_blake2b_final(&hash,digest);return crypto_verify64(digest,j+OTA_BOOT_JOURNAL_HEADER_OFFSET+64);
}
int ota_boot_confirm(int directory,int lock,const uint8_t key[32],const uint8_t id[16],int installed_boot,int healthy,
                     const struct ota_boot_reader *reader,ota_boot_fault hook,void *context,
                     struct ota_boot_result *result,char *error,size_t size)
{
    if(!result)return fail(error,size,"missing result");
    memset(result,0,sizeof *result);
    if(!key||!id||state_lock(directory,lock)||cleanup_temporaries(directory))return fail(error,size,"unsafe confirmation state/lock");
    uint8_t j[OTA_BOOT_JOURNAL_BYTES];size_t length;int rc=record(directory,OTA_BOOT_JOURNAL_NAME,j,sizeof j,&length);
    if(rc==1)return 0;
    struct ota_verified verified;if(rc||length!=sizeof j||journal_valid(j,key,&verified))return fail(error,size,"install journal authentication/map failed");
    result->sequence=verified.sequence;
    if(installed_boot!=1||(healthy!=0&&healthy!=1)||!reader||!reader->read||!nonzero(id,16)||!memcmp(id,j+16,16))return fail(error,size,"new installed-firmware boot witness required");
    uint64_t before;if(accepted(directory,&before)||before>verified.sequence)return fail(error,size,"accepted sequence conflicts with install");
    result->already_accepted=before==verified.sequence;
    if(!result->already_accepted&&!healthy)return fail(error,size,"actual media health required before accepting installed release");
    if(same_record(directory,"pending",verified.header,OTA_HEADER_SIZE,result->already_accepted))return fail(error,size,"pending header differs from install journal");
    if(installed(j,reader))return fail(error,size,"installed package differs from signed payload");
    uint64_t now;if(accepted(directory,&now)||now!=before||same_record(directory,OTA_BOOT_JOURNAL_NAME,j,sizeof j,0)||same_record(directory,"pending",verified.header,OTA_HEADER_SIZE,result->already_accepted))return fail(error,size,"confirmation state changed during readback");
    if(!result->already_accepted){
        char text[32];int n=snprintf(text,sizeof text,"%"PRIu64"\n",verified.sequence);
        if(n<0||(size_t)n>=sizeof text||atomic(directory,"accepted",(const uint8_t *)text,(size_t)n,"accepted",hook,context))return fail(error,size,"accepted commit interrupted; retry confirmation");
    }else if(fsync(directory))return fail(error,size,"accepted recovery sync failed");
    result->accepted_committed=1;
    if(unlinkat(directory,"pending",0)&&errno!=ENOENT)return fail(error,size,"pending cleanup failed");
    if(injected(hook,context,"pending_unlinked")||fsync(directory))return fail(error,size,"pending cleanup sync interrupted");
    if(unlinkat(directory,OTA_BOOT_JOURNAL_NAME,0)||injected(hook,context,"journal_unlinked")||fsync(directory))return fail(error,size,"install journal cleanup interrupted");
    result->cleanup_complete=1;return 1;
}
