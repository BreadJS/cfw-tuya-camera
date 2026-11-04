#define _GNU_SOURCE
#include "wifi.h"
#include "wifi_recovery.h"
#include "wifi_radio.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#define CONFIG_MAX 16384U
struct control_wifi {
    pthread_mutex_t mutex;pthread_t thread;
    int active_dir,persist_dir;char active_name[NAME_MAX+1],socket_path[108],ctrl_dir[108];
    struct camera_wifi_recovery *recovery;int recovery_error;
    uint64_t next_check,apply_at,provision_ready;unsigned provision,frequency;
    unsigned seconds,pending;uint64_t scan_started;uint64_t id,deadline;
    char old_config[CONFIG_MAX+1],new_config[2048],ssid[33];size_t old_bytes,new_bytes;
    int last_error,rollback_error;unsigned confirmed;
    unsigned persistent_changed,persist_existed;
    char old_persist[CONFIG_MAX+1];size_t old_persist_bytes;
    pid_t dhcp;uint64_t dhcp_start;dev_t dhcp_dev;ino_t dhcp_inode;
#ifdef CONTROL_WIFI_TEST
    int (*renew_test)(pid_t);
#endif
};
static uint64_t monotonic(void)
{struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int private_file(int fd)
{struct stat s;if(fstat(fd,&s))return -errno;return !S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)||s.st_nlink!=1?-EPERM:0;}
static int directory(const char *path,unsigned private)
{
    int fd=open(path,O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    struct stat s;int rc=0;
    if(fstat(fd,&s))rc=-errno;
    else if(s.st_uid!=geteuid()||(private?(s.st_mode&077):((s.st_mode&022)&&!(s.st_mode&S_ISVTX))))rc=-EPERM;
    if(rc){close(fd);return rc;}return fd;
}
static int write_all(int fd,const char *data,size_t size)
{while(size){ssize_t n=write(fd,data,size);if(n<0){if(errno==EINTR)continue;return -errno;}if(!n)return -EIO;data+=n;size-=(size_t)n;}return 0;}
/* The directory fd is pinned. Exclusive temporary file, exact mode, fsync,
 * renameat then directory fsync; a post-rename fsync error remains an error. */
static int replace(int dir,const char *name,const char *data,size_t bytes,uint64_t id)
{
    char tmp[80];snprintf(tmp,sizeof tmp,".wifi-%ld-%" PRIu64 ".tmp",(long)getpid(),id);
    int fd=openat(dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    int rc=write_all(fd,data,bytes);if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(dir,tmp,dir,name))rc=-errno;
    if(!rc&&fsync(dir))rc=-errno;
    if(rc)unlinkat(dir,tmp,0);
    return rc;
}
static int read_active(struct control_wifi *w)
{
    int fd=openat(w->active_dir,w->active_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    int rc=private_file(fd);size_t n=0;
    while(!rc&&n<=CONFIG_MAX) {
        ssize_t r=read(fd,w->old_config+n,CONFIG_MAX+1-n);
        if(r<0){if(errno==EINTR)continue;rc=-errno;break;}if(!r)break;n+=(size_t)r;
    }
    if(!rc&&(!n||n>CONFIG_MAX||memchr(w->old_config,0,n)))rc=-EINVAL;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc){w->old_bytes=n;w->old_config[n]=0;}return rc;
}
static int command(struct control_wifi *w,const char *cmd,char *reply,size_t cap)
{
    struct stat st;if(lstat(w->socket_path,&st))return -errno;
    if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid())return -EPERM;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -errno;
    struct sockaddr_un local={.sun_family=AF_UNIX},remote={.sun_family=AF_UNIX};
    static atomic_uint sequence;
    snprintf(local.sun_path,sizeof local.sun_path,"/tmp/.camera-wifi-%ld-%u",(long)getpid(),atomic_fetch_add(&sequence,1)+1);
    memcpy(remote.sun_path,w->socket_path,strlen(w->socket_path)+1);
    int bound=0,rc=0;
    if(bind(fd,(struct sockaddr *)&local,sizeof local))rc=-errno;else bound=1;
    if(!rc&&chmod(local.sun_path,0600))rc=-errno;
    if(!rc&&connect(fd,(struct sockaddr *)&remote,sizeof remote))rc=-errno;
    if(!rc){ssize_t sent=send(fd,cmd,strlen(cmd),MSG_NOSIGNAL);
        if(sent!=(ssize_t)strlen(cmd))rc=sent<0?-errno:-EIO;}
    if(!rc) {
        struct pollfd p={fd,POLLIN,0};int n;
        do{n=poll(&p,1,1000);}while(n<0&&errno==EINTR);
        if(n<=0)rc=n?-errno:-ETIMEDOUT;
        else {
            ssize_t got=recv(fd,reply,cap,MSG_TRUNC);
            if(got<0)rc=-errno;else if((size_t)got>=cap)rc=-EOVERFLOW;else reply[got]=0;
        }
    }
    close(fd);if(bound)unlink(local.sun_path);return rc;
}
static int ack(struct control_wifi *w,const char *cmd,const char *expected)
{char reply[64];int rc=command(w,cmd,reply,sizeof reply);if(rc)return rc;return strcmp(reply,expected)?-EPROTO:0;}
static int process_identity(pid_t pid,uint64_t *start,dev_t *dev,ino_t *ino)
{
    char path[80],text[4096];struct stat st;
    snprintf(path,sizeof path,"/proc/%ld/exe",(long)pid);
    if(stat(path,&st))return -errno;
    *dev=st.st_dev;*ino=st.st_ino;
    struct stat busy;if(stat("/bin/busybox",&busy))return -errno;
    if(st.st_dev!=busy.st_dev||st.st_ino!=busy.st_ino)return -EPERM;
    snprintf(path,sizeof path,"/proc/%ld/cmdline",(long)pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -errno;
    ssize_t n=read(fd,text,sizeof text-1);int saved=errno;close(fd);if(n<=0)return n?-saved:-ESRCH;text[n]=0;
    unsigned foreground=0,wlan=0,client=0;
    for(size_t off=0;off<(size_t)n;) {
        char *p=text+off;size_t len=strnlen(p,(size_t)n-off);if(len==(size_t)n-off)return -EPROTO;
        const char *base=strrchr(p,'/');base=base?base+1:p;
        if(!strcmp(base,"udhcpc"))client=1;
        if(!strcmp(p,"-f"))foreground=1;
        if(!strcmp(p,"-b"))return -EPERM;
        if(!strcmp(p,"-i")) {
            size_t next=off+len+1;if(next>=(size_t)n||strcmp(text+next,"wlan0"))return -EPERM;wlan=1;
        }
        off+=len+1;
    }
    if(!client||!foreground||!wlan)return -EPERM;
    snprintf(path,sizeof path,"/proc/%ld/stat",(long)pid);fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -errno;
    n=read(fd,text,sizeof text-1);saved=errno;close(fd);if(n<=0)return n?-saved:-ESRCH;text[n]=0;
    char *end=strrchr(text,')');if(!end)return -EPROTO;
    char *p=end+2;for(unsigned field=3;field<22;field++){p=strchr(p,' ');if(!p)return -EPROTO;++p;}
    char *tail;errno=0;*start=strtoull(p,&tail,10);return errno||tail==p?-EPROTO:0;
}
static int renew(struct control_wifi *w)
{
    if(!w->dhcp)return 0;
#ifdef CONTROL_WIFI_TEST
    if(w->renew_test)return w->renew_test(w->dhcp);
#endif
    uint64_t start;dev_t dev;ino_t ino;int rc=process_identity(w->dhcp,&start,&dev,&ino);
    if(rc)return rc;
    if(start!=w->dhcp_start||dev!=w->dhcp_dev||ino!=w->dhcp_inode)return -ESTALE;
    return kill(w->dhcp,SIGUSR1)?-errno:0;
}
static int persistent_snapshot(struct control_wifi *w)
{
    int fd=openat(w->persist_dir,"wpa_supplicant.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){if(errno==ENOENT){w->persist_existed=0;return 0;}return -errno;}
    int rc=private_file(fd);size_t n=0;
    while(!rc&&n<=CONFIG_MAX) {
        ssize_t got=read(fd,w->old_persist+n,CONFIG_MAX+1-n);
        if(got<0){if(errno==EINTR)continue;rc=-errno;break;}if(!got)break;n+=(size_t)got;
    }
    if(!rc&&(!n||n>CONFIG_MAX))rc=-EINVAL;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc){w->persist_existed=1;w->old_persist_bytes=n;}return rc;
}
static int rollback(struct control_wifi *w)
{
    int rc=replace(w->active_dir,w->active_name,w->old_config,w->old_bytes,w->id+1);
    if(!rc)rc=ack(w,"RECONFIGURE","OK\n");
    if(!rc)rc=renew(w);
    if(w->persistent_changed) {
        int saved=0;
        if(w->persist_existed)saved=replace(w->persist_dir,"wpa_supplicant.conf",w->old_persist,w->old_persist_bytes,w->id+3);
        else {
            if(unlinkat(w->persist_dir,"wpa_supplicant.conf",0)&&errno!=ENOENT)saved=-errno;
            if(!saved&&fsync(w->persist_dir))saved=-errno;
        }
        if(!saved)w->persistent_changed=0;
        if(!rc)rc=saved;
    }
#ifndef CONTROL_WIFI_TEST
    if(w->provision)camera_wifi_recovery_request(w->recovery,1);
#endif
    w->apply_at=0;w->provision=0;w->provision_ready=0;
    w->rollback_error=rc;w->pending=rc?2U:0U;
    return rc;
}
static int status(struct control_wifi *,char [33],unsigned *);
static void *watch(void *p)
{
    struct control_wifi *w=p;
    for(;;){struct timespec t={.tv_nsec=100000000};nanosleep(&t,NULL);
        pthread_mutex_lock(&w->mutex);uint64_t auto_confirm=0;
        uint64_t now=monotonic();
#ifndef CONTROL_WIFI_TEST
        if(w->apply_at&&now>=w->apply_at) {
            w->apply_at=0;camera_wifi_recovery_request(w->recovery,0);
            int rc=ack(w,"RECONFIGURE","OK\n");if(!rc)rc=renew(w);
            if(rc){w->last_error=rc;rollback(w);}
        }
        if(now>=w->next_check) {
            w->next_check=now+1000;char ssid[33];unsigned complete=0;
            int rc=status(w,ssid,&complete);int connected=!rc&&complete&&camera_wifi_station_ready();
            if(connected) {
                char signal[512];if(!command(w,"SIGNAL_POLL",signal,sizeof signal)){char *f=strstr(signal,"FREQUENCY=");if(f)w->frequency=(unsigned)atoi(f+10);}
            }
            if(w->provision&&w->pending==1&&connected&&!strcmp(ssid,w->ssid)) {
                if(!w->provision_ready)w->provision_ready=now;
                if(now-w->provision_ready>=5000)auto_confirm=w->id;
            }else w->provision_ready=0;
            unsigned channel=w->frequency>=2412&&w->frequency<=2462?(w->frequency-2407)/5:6;
            w->recovery_error=camera_wifi_recovery_tick(w->recovery,now,connected,channel,w->pending!=0);
        }
#endif
        if(w->pending==1&&(!now||now>=w->deadline)){w->apply_at=0;if(!now)w->last_error=-EIO;rollback(w);}
        pthread_mutex_unlock(&w->mutex);
        if(auto_confirm)control_wifi_confirm(w,auto_confirm);
    }return NULL;
}
int control_wifi_init(struct control_wifi **out,const struct control_wifi_options *o)
{
    if(!out||*out||!o||!o->active_config||!o->control_socket||!o->persistent_directory||o->dhcp_pid==1||o->dhcp_pid<0)return -EINVAL;
#ifndef CONTROL_WIFI_TEST
    if(geteuid()!=0)return -EPERM;
#endif
    const char *slash=strrchr(o->active_config,'/');size_t len=strlen(o->active_config);
    if(!slash||slash==o->active_config||!slash[1]||len>=PATH_MAX||strlen(slash+1)>NAME_MAX||strlen(o->control_socket)>=108||o->control_socket[0]!='/')return -EINVAL;
    for(const char*p=o->control_socket;*p;p++)if(!(isalnum((unsigned char)*p)||strchr("/_-.",*p)))return -EINVAL;
    struct control_wifi *w=calloc(1,sizeof *w);if(!w)return -ENOMEM;
    w->active_dir=w->persist_dir=-1;w->seconds=90;w->dhcp=o->dhcp_pid;
#ifdef CONTROL_WIFI_TEST
    if(o->confirmation_seconds){if(o->confirmation_seconds<1||o->confirmation_seconds>90){free(w);return -EINVAL;}w->seconds=o->confirmation_seconds;}
    w->renew_test=o->renew_test;
#endif
    char parent[PATH_MAX];memcpy(parent,o->active_config,(size_t)(slash-o->active_config));parent[slash-o->active_config]=0;
    if(!strcmp(slash+1,".camera-wifi-previous.conf")){free(w);return -EINVAL;}
    strcpy(w->active_name,slash+1);strcpy(w->socket_path,o->control_socket);
    strcpy(w->ctrl_dir,o->control_socket);char *last=strrchr(w->ctrl_dir,'/');*last=0;
    int rc=directory(parent,0);if(rc<0)goto fail;w->active_dir=rc;
#ifndef CONTROL_WIFI_TEST
    struct statfs fs;
    if(fstatfs(w->active_dir,&fs)){rc=-errno;goto fail;}
    if((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL){rc=-EPERM;goto fail;}
#endif
    rc=directory(o->persistent_directory,1);if(rc<0)goto fail;w->persist_dir=rc;
    rc=read_active(w);if(rc)goto fail;
    if(w->dhcp) {
#ifdef CONTROL_WIFI_TEST
        if(!w->renew_test)
#endif
        {rc=process_identity(w->dhcp,&w->dhcp_start,&w->dhcp_dev,&w->dhcp_inode);if(rc)goto fail;}
    }
    rc=pthread_mutex_init(&w->mutex,NULL);if(rc){rc=-rc;goto fail;}
    rc=ack(w,"PING","PONG\n");if(rc){pthread_mutex_destroy(&w->mutex);goto fail;}
#ifndef CONTROL_WIFI_TEST
    w->recovery_error=camera_wifi_recovery_open(&w->recovery,w->persist_dir);
#endif
    rc=pthread_create(&w->thread,NULL,watch,w);if(rc){pthread_mutex_destroy(&w->mutex);rc=-rc;goto fail;}
    *out=w;return 0;
fail:
    if(w->active_dir>=0)close(w->active_dir);
    if(w->persist_dir>=0)close(w->persist_dir);
    free(w);return rc;
}
int control_wifi_begin(struct control_wifi *w,const char *ssid,const char *password,uint64_t *id)
{
    if(!w||!ssid||!password||!id)return -EINVAL;
    size_t n=strlen(ssid),p=strlen(password);if(!n||n>32||(p&&p<8)||p>64)return -EINVAL;
    for(size_t i=0;i<p;i++)if(p==64?!isxdigit((unsigned char)password[i]):((unsigned char)password[i]<32||(unsigned char)password[i]>126||password[i]=='"'||password[i]=='\\'))return -EINVAL;
    char hex[65];static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){hex[2*i]=digits[(unsigned char)ssid[i]>>4];hex[2*i+1]=digits[(unsigned char)ssid[i]&15];}hex[2*n]=0;
    pthread_mutex_lock(&w->mutex);int rc=0;
    if(w->pending){rc=-EBUSY;goto done;}
    rc=read_active(w);if(rc)goto done;
    uint64_t now=monotonic();if(!now){rc=-EIO;goto done;}
    w->id=now>w->id?now:w->id+4;w->confirmed=0;w->rollback_error=0;
    int bytes=snprintf(w->new_config,sizeof w->new_config,"ctrl_interface=%s\nupdate_config=0\nap_scan=1\nnetwork={\n ssid=%s\n psk=%s%s%s\n key_mgmt=WPA-PSK\n}\n",w->ctrl_dir,hex,p==64?"":"\"",password,p==64?"":"\"");
    if(!p)bytes=snprintf(w->new_config,sizeof w->new_config,"ctrl_interface=%s\nupdate_config=0\nap_scan=1\nnetwork={\n ssid=%s\n key_mgmt=NONE\n}\n",w->ctrl_dir,hex);
    if(bytes<0||(size_t)bytes>=sizeof w->new_config){rc=-EOVERFLOW;goto done;}w->new_bytes=(size_t)bytes;
    strcpy(w->ssid,ssid);
    rc=replace(w->active_dir,".camera-wifi-previous.conf",w->old_config,w->old_bytes,w->id);if(rc)goto done;
    /* From this point failures can mutate active state, so arm rollback first. */
    w->pending=1;w->deadline=now+(uint64_t)w->seconds*1000;*id=w->id;
    rc=replace(w->active_dir,w->active_name,w->new_config,w->new_bytes,w->id+1);
#ifndef CONTROL_WIFI_TEST
    w->provision=camera_wifi_recovery_active(w->recovery);w->provision_ready=0;
    if(!rc&&w->provision)w->apply_at=now+1800;
#endif
    if(!rc&&!w->apply_at)rc=ack(w,"RECONFIGURE","OK\n");
    if(!rc&&!w->apply_at)rc=renew(w);
    if(rc){w->last_error=rc;rollback(w);}
done:
    w->last_error=rc;pthread_mutex_unlock(&w->mutex);return rc;
}
static int hexval(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static int status(struct control_wifi*w,char ssid[33],unsigned *completed)
{
    char reply[4096];int rc=command(w,"STATUS",reply,sizeof reply);if(rc)return rc;
    ssid[0]=0;*completed=0;
    for(char *p=reply;*p;) {
        char *end=strchr(p,'\n');if(end)*end=0;
        if(!strcmp(p,"wpa_state=COMPLETED"))*completed=1;
        if(!strncmp(p,"freq=",5))w->frequency=(unsigned)atoi(p+5);
        if(!strncmp(p,"ssid=",5)) {
            const char *r=p+5;size_t n=0;
            while(*r) {
                unsigned char c=(unsigned char)*r++;
                if(c=='\\') {
                    c=(unsigned char)*r++;if(!c)return -EPROTO;
                    if(c=='x'){if(!r[0]||!r[1]||hexval(r[0])<0||hexval(r[1])<0)return -EPROTO;c=(unsigned char)(hexval(r[0])*16+hexval(r[1]));r+=2;}
                    else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='e')c=27;else if(c!='\\'&&c!='"')return -EPROTO;
                }
                if(!c||n>=32)return -EPROTO;
                ssid[n++]=(char)c;
            }ssid[n]=0;
        }
        if(!end)break;
        p=end+1;
    }return 0;
}
int control_wifi_confirm(struct control_wifi *w,uint64_t id)
{
    if(!w)return -EINVAL;
    pthread_mutex_lock(&w->mutex);int rc=0;char ssid[33];unsigned complete;
    if(w->pending!=1||w->id!=id){rc=-ESTALE;goto done;}
    if(monotonic()>=w->deadline){rc=-ETIMEDOUT;rollback(w);goto done;}
    rc=status(w,ssid,&complete);if(rc)goto done;
    if(!complete||strcmp(ssid,w->ssid)){rc=-EAGAIN;goto done;}
    if(monotonic()>=w->deadline){rc=-ETIMEDOUT;rollback(w);goto done;}
    rc=persistent_snapshot(w);if(rc)goto done;
    w->persistent_changed=1;
    rc=replace(w->persist_dir,"wpa_supplicant.conf",w->new_config,w->new_bytes,w->id+2);
    if(!rc){w->persistent_changed=0;w->pending=0;w->confirmed=1;}
    else rollback(w);
done:w->last_error=rc;pthread_mutex_unlock(&w->mutex);return rc;
}
int control_wifi_cancel(struct control_wifi *w,uint64_t id)
{
    if(!w)return -EINVAL;
    pthread_mutex_lock(&w->mutex);
    int rc=!w->pending||id!=w->id?-ESTALE:rollback(w);w->last_error=rc;
    pthread_mutex_unlock(&w->mutex);return rc;
}
static int json_ssid(char*out,size_t cap,const char*ssid)
{
    static const char hex[]="0123456789abcdef";size_t n=0;
    if(cap<3)return -ENOSPC;
    out[n++]='"';
    for(const unsigned char*p=(const unsigned char*)ssid;*p;p++) {
        if(*p<32||*p>=127||*p=='"'||*p=='\\') {
            if(cap-n<7)return -ENOSPC;
            memcpy(out+n,"\\u00",4);out[n+4]=hex[*p>>4];out[n+5]=hex[*p&15];n+=6;
        }else {if(cap-n<3)return -ENOSPC;out[n++]=(char)*p;}
    }out[n++]='"';out[n]=0;return 0;
}
int control_wifi_status_json(struct control_wifi*w,char*out,size_t cap)
{
    if(!w||!out||!cap)return -EINVAL;
    pthread_mutex_lock(&w->mutex);
    char ssid[33]={0},quoted[200];unsigned complete=0;int rc=status(w,ssid,&complete);
    int error=json_ssid(quoted,sizeof quoted,ssid);uint64_t now=monotonic();
    uint64_t remaining=w->pending==1&&w->deadline>now?(w->deadline-now+999)/1000:0;
    if(!error){int n=snprintf(out,cap,"{\"ssid\":%s,\"completed\":%s,\"pending\":%s,\"transaction\":\"%" PRIu64 "\",\"remaining_seconds\":%" PRIu64 ",\"confirmed\":%s,\"status_error\":%d,\"last_error\":%d,\"rollback_error\":%d,\"rollback_failed\":%s}",quoted,complete?"true":"false",w->pending?"true":"false",w->id,remaining,w->confirmed?"true":"false",rc,w->last_error,w->rollback_error,w->pending==2?"true":"false");if(n<0||(size_t)n>=cap)error=-ENOSPC;}
    pthread_mutex_unlock(&w->mutex);return error;
}

int control_wifi_scan_begin(struct control_wifi *w)
{
    if(!w)return -EINVAL;
    pthread_mutex_lock(&w->mutex);
    uint64_t now=monotonic();int rc=0;
    if(w->scan_started&&now-w->scan_started<5000)rc=-EBUSY;
    else {rc=ack(w,"SCAN","OK\n");if(!rc)w->scan_started=now;}
    pthread_mutex_unlock(&w->mutex);return rc;
}
int control_wifi_scan_json(struct control_wifi *w,char *out,size_t cap)
{
    if(!w||!out||cap<128)return -EINVAL;
    pthread_mutex_lock(&w->mutex);
    char reply[16384];int rc=command(w,"SCAN_RESULTS",reply,sizeof reply);
    if(rc){pthread_mutex_unlock(&w->mutex);return rc;}
    uint64_t now=monotonic();unsigned scanning=w->scan_started&&now-w->scan_started<4000;
    size_t used=(size_t)snprintf(out,cap,"{\"scanning\":%s,\"networks\":[",scanning?"true":"false");
    unsigned count=0;char *line=strchr(reply,'\n');
    while(line&&*++line&&count<24) {
        char *next=strchr(line,'\n');if(next)*next=0;
        char *fields[5]={line};unsigned n=1;
        for(char *p=line;*p&&n<5;p++)if(*p=='\t'){*p=0;fields[n++]=p+1;}
        if(n==5) {
            char ssid[33];size_t k=0;int valid=1;
            for(const char *p=fields[4];*p;) {
                unsigned char c=(unsigned char)*p++;
                if(c=='\\') {c=(unsigned char)*p++;if(!c){valid=0;break;}if(c=='x'){if(!p[0]||!p[1]||hexval(p[0])<0||hexval(p[1])<0){valid=0;break;}c=(unsigned char)(hexval(p[0])*16+hexval(p[1]));p+=2;}else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='e')c=27;else if(c!='\\'&&c!='"'){valid=0;break;}}
                if(!c||k>=32){valid=0;break;}ssid[k++]=(char)c;
            }ssid[k]=0;
            char quoted[200],flags[200];int frequency=atoi(fields[1]),signal=atoi(fields[2]);
            if(valid&&strlen(fields[3])<100&&!json_ssid(quoted,sizeof quoted,ssid)&&!json_ssid(flags,sizeof flags,fields[3])&&frequency>=2300&&frequency<=7100&&signal>=-200&&signal<=0) {
                int wrote=snprintf(out+used,cap-used,"%s{\"ssid\":%s,\"signal_dbm\":%d,\"frequency\":%d,\"security\":%s,\"hidden\":%s}",count?",":"",quoted,signal,frequency,flags,k?"false":"true");
                if(wrote<0||(size_t)wrote>=cap-used-3)break;
                used+=(size_t)wrote;++count;
            }
        }
        line=next;
    }
    if(used+3>cap)rc=-ENOSPC;else{memcpy(out+used,"]}",3);rc=0;}
    pthread_mutex_unlock(&w->mutex);return rc;
}

int control_wifi_recovery_json(struct control_wifi *w,char *out,size_t cap,int include_password)
{
    if(!w||!out)return -EINVAL;
    pthread_mutex_lock(&w->mutex);
    int rc;
#ifdef CONTROL_WIFI_TEST
    (void)include_password;(void)cap;rc=-ENOTSUP;
#else
    if(!w->recovery)rc=-ENOTSUP;else rc=camera_wifi_recovery_json(w->recovery,out,cap,include_password);
#endif
    pthread_mutex_unlock(&w->mutex);return rc;
}
int control_wifi_recovery_request(struct control_wifi *w,unsigned enable)
{
    if(!w||enable>1)return -EINVAL;
    pthread_mutex_lock(&w->mutex);int rc;
#ifdef CONTROL_WIFI_TEST
    rc=-ENOTSUP;
#else
    rc=w->pending?-EBUSY:camera_wifi_recovery_request(w->recovery,(int)enable);
#endif
    pthread_mutex_unlock(&w->mutex);return rc;
}
