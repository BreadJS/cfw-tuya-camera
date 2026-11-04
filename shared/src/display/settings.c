#define _GNU_SOURCE
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static atomic_uint serial;
static int directory(int fd)
{struct stat s;return fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)?-1:0;}
static int private_file(const struct stat *s)
{return !S_ISREG(s->st_mode)||s->st_uid!=geteuid()||(s->st_mode&07777)!=0600||s->st_nlink!=1;}
void camera_display_defaults(struct camera_display_settings *s)
{if(s){memset(s,0,sizeof *s);strcpy(s->name,"Camera");}}
/* Strict Unicode scalar UTF-8: reject overlong sequences, surrogate codepoints,
 * missing continuations and codepoints beyond U+10FFFF. */
static int utf8(const unsigned char *p,size_t n)
{
    for(size_t at=0;at<n;){
        unsigned c=p[at++],value,min,extra;
        if(c<0x80){if(c<32||c==127)return 0;continue;}
        if(c>=0xc2&&c<=0xdf){value=c&31;min=0x80;extra=1;}
        else if(c>=0xe0&&c<=0xef){value=c&15;min=0x800;extra=2;}
        else if(c>=0xf0&&c<=0xf4){value=c&7;min=0x10000;extra=3;}
        else return 0;
        if(n-at<extra)return 0;
        while(extra--){unsigned next=p[at++];if((next&0xc0)!=0x80)return 0;value=(value<<6)|(next&63);}
        if(value<min||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return 0;
    }
    return 1;
}
int camera_display_valid(const struct camera_display_settings *s)
{
    if(!s||s->timestamp_enabled>1||s->timezone>1)return 0;
    const char *end=memchr(s->name,0,sizeof s->name);
    return end&&end!=s->name&&utf8((const unsigned char *)s->name,(size_t)(end-s->name));
}
static uint32_t get(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,unsigned v)
{p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int camera_display_load(int dir,struct camera_display_settings *out)
{
    if(!out||directory(dir))return -1;
    int fd=openat(dir,"display.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){if(errno!=ENOENT)return -1;camera_display_defaults(out);return 0;}
    unsigned char b[80];struct stat before,after;int rc=-1;
    if(fstat(fd,&before)||private_file(&before)||before.st_size!=sizeof b)goto done;
    size_t done=0;
    while(done<sizeof b){ssize_t n=read(fd,b+done,sizeof b-done);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;done+=(size_t)n;}
    if(fstat(fd,&after)||private_file(&after)||after.st_size!=before.st_size||
       after.st_mtim.tv_sec!=before.st_mtim.tv_sec||after.st_mtim.tv_nsec!=before.st_mtim.tv_nsec||
       after.st_ctim.tv_sec!=before.st_ctim.tv_sec||after.st_ctim.tv_nsec!=before.st_ctim.tv_nsec||memcmp(b,"CAMDSP1\0",8))goto done;
    struct camera_display_settings s={.timestamp_enabled=get(b+8),.timezone=get(b+12)};memcpy(s.name,b+16,64);
    if(!camera_display_valid(&s))goto done;
    size_t length=strlen(s.name);
    for(size_t i=length+1;i<64;i++)if(s.name[i])goto done;
    if(close(fd))return -1;
    *out=s;return 0;
done:
    close(fd);return rc;
}
int camera_display_save(int dir,const struct camera_display_settings *s)
{
    if(directory(dir)||!camera_display_valid(s))return -1;
    struct stat old;
    if(!fstatat(dir,"display.conf",&old,AT_SYMLINK_NOFOLLOW)){if(private_file(&old))return -1;}
    else if(errno!=ENOENT)return -1;
    unsigned char b[80]={0};memcpy(b,"CAMDSP1\0",8);put(b+8,s->timestamp_enabled);put(b+12,s->timezone);memcpy(b+16,s->name,strlen(s->name));
    char name[80];int fd=-1;
    for(unsigned attempt=0;attempt<100;attempt++){
        unsigned next=atomic_fetch_add(&serial,1)+1;
        int n=snprintf(name,sizeof name,"display.tmp.%ld.%u",(long)getpid(),next);if(n<0||(size_t)n>=sizeof name)return -1;
        fd=openat(dir,name,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0)break;
        if(errno!=EEXIST)return -1;
    }
    if(fd<0)return -1;
    int rc=-1;size_t done=0;
    if(fchmod(fd,0600))goto done;
    while(done<sizeof b){ssize_t n=write(fd,b+done,sizeof b-done);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;done+=(size_t)n;}
    if(fsync(fd))goto done;
    if(close(fd)){fd=-1;goto done;}fd=-1;
    if(renameat(dir,name,dir,"display.conf")||fsync(dir))goto done;
    rc=0;
done:
    if(fd>=0)close(fd);
    if(rc)unlinkat(dir,name,0);
    return rc;
}
static int transition(int year,int month,time_t *out)
{
    struct tm t={.tm_year=year,.tm_mon=month,.tm_mday=31,.tm_hour=1};
    errno=0;time_t last=timegm(&t);if(last==(time_t)-1&&errno)return -1;
    struct tm day;if(!gmtime_r(&last,&day))return -1;
    t.tm_mday=31-day.tm_wday;errno=0;*out=timegm(&t);
    return *out==(time_t)-1&&errno?-1:0;
}
int camera_display_format_clock(time_t now,unsigned timezone,char out[24])
{
    if(!out||timezone>1)return -1;
    struct tm utc;if(!gmtime_r(&now,&utc))return -1;
    time_t local=now;
    if(!timezone){
        time_t start,end;if(transition(utc.tm_year,2,&start)||transition(utc.tm_year,9,&end))return -1;
        time_t offset=now>=start&&now<end?7200:3600;
        if(__builtin_add_overflow(now,offset,&local))return -1;
    }
    struct tm t;if(!gmtime_r(&local,&t)||t.tm_year < -1900||t.tm_year>8099||t.tm_wday<0||t.tm_wday>6)return -1;
    static const char *const days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    char b[24];int n=snprintf(b,sizeof b,"%04d-%02d-%02d %s %02d:%02d:%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday,days[t.tm_wday],t.tm_hour,t.tm_min,t.tm_sec);
    if(n!=23)return -1;
    memcpy(out,b,24);return 0;
}
