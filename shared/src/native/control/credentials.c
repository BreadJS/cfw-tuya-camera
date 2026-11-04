#define _GNU_SOURCE
#include "credentials.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static char pair[2][256];
static unsigned generations[2];
static int store=-1;
int camera_credentials_verify(unsigned role,const char *user,const char *password)
{
    if(role>1||!user||!password||!user[0]||strchr(user,':')||strlen(user)>63||strlen(password)>127)return 0;
    char candidate[256]={0},expected[256]={0};
    int n=snprintf(candidate,sizeof candidate,"%s:%s",user,password);
    if(n<0||(size_t)n>=sizeof candidate)return 0;
    pthread_mutex_lock(&lock);int ready=store>=0;
    if(ready)memcpy(expected,pair[role],strlen(pair[role])+1);
    pthread_mutex_unlock(&lock);
    unsigned different=0;for(unsigned i=0;i<sizeof candidate;i++)different|=(unsigned char)candidate[i]^(unsigned char)expected[i];
    volatile char *a=candidate,*b=expected;for(unsigned i=0;i<sizeof candidate;i++){a[i]=0;b[i]=0;}
    return ready&&!different;
}
static int valid(const char *text)
{
    const char *colon=strchr(text,':');
    if(!colon||colon==text||colon-text>63||!colon[1]||strlen(colon+1)>127)return 0;
    for(const unsigned char *p=(const unsigned char *)text;*p;p++)if(*p<32||*p>126)return 0;
    return 1;
}
int camera_credentials_open(int directory,const char *initial)
{
    if(directory<0||!initial||strlen(initial)>=256||!valid(initial))return -EINVAL;
    char loaded[2][256]={{0}};strcpy(loaded[0],initial);strcpy(loaded[1],initial);
    int fd=openat(directory,"credentials.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        unsigned char data[521];struct stat st;ssize_t n=read(fd,data,sizeof data);
        int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1;
        close(fd);
        if(bad||n!=520||memcmp(data,"CAMAUTH1",8))return -EPROTO;
        memcpy(loaded,data+8,512);
        for(unsigned i=0;i<2;i++)if(!memchr(loaded[i],0,256)||!valid(loaded[i]))return -EPROTO;
    }else if(errno!=ENOENT)return -errno;
    int duplicate=fcntl(directory,F_DUPFD_CLOEXEC,3);if(duplicate<0)return -errno;
    pthread_mutex_lock(&lock);if(store>=0)close(store);store=duplicate;
    memcpy(pair,loaded,sizeof pair);++generations[0];++generations[1];pthread_mutex_unlock(&lock);
    memset(loaded,0,sizeof loaded);return 0;
}
int camera_credentials_token(unsigned role,char *out,size_t size,unsigned *generation)
{
    if(role>1||!out||size<350)return -EINVAL;
    pthread_mutex_lock(&lock);if(store<0){pthread_mutex_unlock(&lock);return 0;}
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p=(const unsigned char *)pair[role];size_t length=strlen(pair[role]),k=6;memcpy(out,"Basic ",6);
    for(size_t i=0;i<length;i+=3){unsigned v=p[i]<<16;if(i+1<length)v|=p[i+1]<<8;if(i+2<length)v|=p[i+2];out[k++]=alphabet[v>>18];out[k++]=alphabet[v>>12&63];out[k++]=i+1<length?alphabet[v>>6&63]:'=';out[k++]=i+2<length?alphabet[v&63]:'=';}
    out[k]=0;if(generation)*generation=generations[role];pthread_mutex_unlock(&lock);return 1;
}
int camera_credentials_user(unsigned role,char *out,size_t size)
{
    if(role>1||!out)return -EINVAL;
    pthread_mutex_lock(&lock);const char *colon=strchr(pair[role],':');size_t n=colon?(size_t)(colon-pair[role]):0;
    if(store<0||!n||n>=size){pthread_mutex_unlock(&lock);return -EINVAL;}
    memcpy(out,pair[role],n);out[n]=0;pthread_mutex_unlock(&lock);return 0;
}
int camera_credentials_change(unsigned role,const char *user,const char *password)
{
    if(role>1||!user||!password||strchr(user,':')||strlen(user)>63||strlen(password)>127)return -EINVAL;
    char next[256];int length=snprintf(next,sizeof next,"%s:%s",user,password);if(length<0||length>=(int)sizeof next||!valid(next))return -EINVAL;
    pthread_mutex_lock(&lock);if(store<0){pthread_mutex_unlock(&lock);return -EINVAL;}
    unsigned char data[520]={0};memcpy(data,"CAMAUTH1",8);memcpy(data+8,pair,512);memset(data+8+role*256,0,256);memcpy(data+8+role*256,next,(size_t)length);
    char name[64];snprintf(name,sizeof name,"credentials.tmp.%ld",(long)getpid());
    int fd=openat(store,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600),rc=0,renamed=0;
    if(fd<0)rc=-errno;
    else {
        size_t done=0;while(done<sizeof data){ssize_t n=write(fd,data+done,sizeof data-done);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}done+=(size_t)n;}
        if(!rc&&fsync(fd))rc=-errno;
        if(close(fd)&&!rc)rc=-errno;
        if(!rc){if(renameat(store,name,store,"credentials.conf"))rc=-errno;else renamed=1;}
        if(renamed){memcpy(pair[role],data+8+role*256,256);++generations[role];if(fsync(store))rc=-errno;}
        if(!renamed)unlinkat(store,name,0);
    }
    memset(data,0,sizeof data);memset(next,0,sizeof next);pthread_mutex_unlock(&lock);return rc;
}
