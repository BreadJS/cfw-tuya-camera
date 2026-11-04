#define _GNU_SOURCE
#include "wifi_recovery.h"
#include "wifi_radio.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#ifndef CAMERA_WIFI_RAM_PATH
#define CAMERA_WIFI_RAM_PATH "/var/tmp/camera-wifi"
#endif
struct camera_wifi_recovery {
    int directory,ram,ready,owned,error,forced,enabled;
    pid_t ap,dhcp;uint64_t offline,next_attempt,healthy;
    char ssid[33],password[65];
};
static int private_write(int dir,const char *name,const void *data,size_t size)
{
    char temporary[80];snprintf(temporary,sizeof temporary,"%s.tmp.%ld",name,(long)getpid());
    int fd=openat(dir,temporary,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -errno;
    int rc=0;const char *p=data;
    while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0){rc=-EIO;break;}p+=n;size-=(size_t)n;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&renameat(dir,temporary,dir,name))rc=-errno;
    if(!rc&&fsync(dir))rc=-errno;
    if(rc)unlinkat(dir,temporary,0);
    return rc;
}
static int secrets(struct camera_wifi_recovery *s)
{
    unsigned char data[106]={0};struct stat st;
    int fd=openat(s->directory,"recovery.conf",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){ssize_t n=read(fd,data,sizeof data);unsigned char extra;ssize_t more=read(fd,&extra,1);int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1;close(fd);
        if(n!=sizeof data||more!=0||bad||memcmp(data,"CAMAP001",8)||!memchr(data+8,0,33)||!memchr(data+41,0,65))return -EPROTO;
        memcpy(s->ssid,data+8,33);memcpy(s->password,data+41,65);
        if(strncmp(s->ssid,"Camera-Setup-",13)||strlen(s->ssid)!=19)return -EPROTO;
        for(const char *p=s->ssid+13;*p;p++)if(!strchr("0123456789abcdef",*p))return -EPROTO;
        if(strcmp(s->password,"camera123")) {
            memset(s->password,0,sizeof s->password);strcpy(s->password,"camera123");
            memset(data+41,0,65);memcpy(data+41,s->password,65);
            int rc=private_write(s->directory,"recovery.conf",data,sizeof data);if(rc)return rc;
        }
    }else{
        if(errno!=ENOENT)return -errno;
        unsigned char random[3];ssize_t n=getrandom(random,sizeof random,GRND_NONBLOCK);if(n!=sizeof random)return n<0?-errno:-EIO;
        static const char hex[]="0123456789abcdef";strcpy(s->ssid,"Camera-Setup-");
        for(unsigned i=0;i<3;i++){s->ssid[13+i*2]=hex[random[i]>>4];s->ssid[14+i*2]=hex[random[i]&15];}s->ssid[19]=0;
        strcpy(s->password,"camera123");
        memcpy(data,"CAMAP001",8);memcpy(data+8,s->ssid,33);memcpy(data+41,s->password,65);
        int rc=private_write(s->directory,"recovery.conf",data,sizeof data);memset(data,0,sizeof data);if(rc)return rc;
    }s->ready=1;return 0;
}
static int child(struct camera_wifi_recovery *s,const char *log,const char *executable,char *const argv[],pid_t *pid)
{
    int fd=openat(s->ram,log,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -errno;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1){close(fd);return -EPERM;}
    posix_spawn_file_actions_t actions;int rc=posix_spawn_file_actions_init(&actions);
    if(rc){close(fd);return -rc;}
    rc=posix_spawn_file_actions_adddup2(&actions,fd,1);if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,fd,2);
    if(!rc)rc=posix_spawn(pid,executable,&actions,NULL,argv,environ);
    posix_spawn_file_actions_destroy(&actions);close(fd);return -rc;
}
static void reap(pid_t *pid)
{if(*pid>0){pid_t n=waitpid(*pid,NULL,WNOHANG);if(n==*pid||(n<0&&errno==ECHILD))*pid=0;}}
static void stop_child(pid_t *pid)
{
    reap(pid);if(*pid<=0)return;
    /* An unreaped child PID cannot be reused. Never signal a pidfile value. */
    kill(*pid,SIGTERM);
    for(unsigned i=0;i<20;i++){reap(pid);if(!*pid)return;struct timespec delay={.tv_nsec=25000000};nanosleep(&delay,NULL);}
    kill(*pid,SIGKILL);while(waitpid(*pid,NULL,0)<0&&errno==EINTR){}*pid=0;
}
int camera_wifi_recovery_stop(struct camera_wifi_recovery *s)
{
    if(!s)return -EINVAL;
    s->enabled=0;stop_child(&s->dhcp);stop_child(&s->ap);
    int rc=0;if(s->owned){rc=camera_wifi_ap_delete();if(rc==-ENODEV)rc=0;if(!rc)s->owned=0;}return rc;
}
static int start(struct camera_wifi_recovery *s,unsigned channel)
{
    if(!s->ready)return -EAGAIN;
    if(channel<1||channel>11)channel=6;
    char config[1024];int n=snprintf(config,sizeof config,"interface=cameraap0\ndriver=nl80211\nctrl_interface=/var/tmp/camera-wifi/hostapd\nssid=%s\nhw_mode=g\nchannel=%u\ncountry_code=NL\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\nwpa_passphrase=%s\nauth_algs=1\nap_isolate=1\nmax_num_sta=4\n",s->ssid,channel,s->password);
    if(n<0||n>=(int)sizeof config)return -EOVERFLOW;
    int rc=private_write(s->ram,"hostapd.conf",config,(size_t)n);memset(config,0,sizeof config);if(rc)return rc;
    static const char dhcp[]="start 192.168.10.20\nend 192.168.10.40\ninterface cameraap0\nmax_leases 4\nlease_file /var/tmp/camera-wifi/leases\npidfile /var/tmp/camera-wifi/udhcpd.pid\noption subnet 255.255.255.0\noption lease 600\n";
    rc=private_write(s->ram,"udhcpd.conf",dhcp,sizeof dhcp-1);if(rc)return rc;
    rc=camera_wifi_ap_create();if(rc)return rc;s->owned=1;
    rc=camera_wifi_ap_address();if(rc)goto failed;
    char *ap_argv[]={"hostapd","/var/tmp/camera-wifi/hostapd.conf",NULL};
    rc=child(s,"hostapd.log","/bin/hostapd",ap_argv,&s->ap);if(rc)goto failed;
    char *dhcp_argv[]={"busybox","udhcpd","-f","/var/tmp/camera-wifi/udhcpd.conf",NULL};
    rc=child(s,"udhcpd.log","/bin/busybox",dhcp_argv,&s->dhcp);if(rc)goto failed;
    return 0;
failed:camera_wifi_recovery_stop(s);return rc;
}
int camera_wifi_recovery_open(struct camera_wifi_recovery **out,int directory)
{
    if(!out||*out||directory<0)return -EINVAL;
    if(mkdir(CAMERA_WIFI_RAM_PATH,0700)&&errno!=EEXIST)return -errno;
    int ram=open(CAMERA_WIFI_RAM_PATH,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(ram<0)return -errno;
    struct stat st;if(fstat(ram,&st)||st.st_uid!=geteuid()||(st.st_mode&077)){close(ram);return -EPERM;}
    struct camera_wifi_recovery *s=calloc(1,sizeof *s);if(!s){close(ram);return -ENOMEM;}
    s->ram=ram;s->directory=fcntl(directory,F_DUPFD_CLOEXEC,3);if(s->directory<0){close(ram);free(s);return -errno;}
    s->error=secrets(s);*out=s;return 0;
}
int camera_wifi_recovery_request(struct camera_wifi_recovery *s,int enable)
{if(!s)return -EINVAL;s->forced=enable?1:0;if(!enable)return camera_wifi_recovery_stop(s);s->next_attempt=0;return 0;}
int camera_wifi_recovery_active(struct camera_wifi_recovery *s)
{return s&&s->ap>0&&s->dhcp>0&&s->enabled;}
static int enabled(struct camera_wifi_recovery *s)
{
    if(s->ap<=0||s->dhcp<=0)return 0;
    struct sockaddr_un local={.sun_family=AF_UNIX},remote={.sun_family=AF_UNIX};
    static unsigned serial;snprintf(local.sun_path,sizeof local.sun_path,"/var/tmp/camera-wifi/check-%ld-%u",(long)getpid(),++serial);
    strcpy(remote.sun_path,"/var/tmp/camera-wifi/hostapd/cameraap0");
    struct stat st;if(lstat(remote.sun_path,&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid())return 0;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return 0;
    int bound=0,ready=0;
    if(!bind(fd,(struct sockaddr *)&local,sizeof local)) {
        bound=1;chmod(local.sun_path,0600);
        if(!connect(fd,(struct sockaddr *)&remote,sizeof remote)&&send(fd,"STATUS",6,MSG_NOSIGNAL)==6) {
            struct pollfd p={fd,POLLIN,0};if(poll(&p,1,300)>0){char reply[4096];ssize_t n=recv(fd,reply,sizeof reply-1,0);if(n>0){reply[n]=0;ready=!strncmp(reply,"state=ENABLED\n",14)||strstr(reply,"\nstate=ENABLED\n")!=NULL;}}
        }
    }
    close(fd);if(bound)unlink(local.sun_path);return ready;
}
int camera_wifi_recovery_tick(struct camera_wifi_recovery *s,uint64_t now,int connected,unsigned channel,int pending)
{
    if(!s)return -ENOTSUP;
    if(!s->ready){s->error=secrets(s);if(!s->ready)return s->error;}
    if(s->ap>0||s->dhcp>0){reap(&s->ap);reap(&s->dhcp);if(!s->ap||!s->dhcp){camera_wifi_recovery_stop(s);s->error=-ECHILD;s->next_attempt=now+30000;}}
    s->enabled=enabled(s);
    if(connected){s->offline=0;if(!s->healthy)s->healthy=now;if(!pending&&!s->forced&&now-s->healthy>=10000&&s->owned)camera_wifi_recovery_stop(s);}
    else{s->healthy=0;if(!s->offline)s->offline=now;}
    if(!s->owned&&!pending&&(s->forced||(!connected&&now-s->offline>=45000))&&now>=s->next_attempt){s->error=start(s,channel);s->next_attempt=now+30000;}
    return s->error;
}
int camera_wifi_recovery_json(struct camera_wifi_recovery *s,char *out,size_t cap,int include_password)
{
    if(!s||!out)return -EINVAL;
    int n=snprintf(out,cap,"{\"available\":true,\"ready\":%s,\"active\":%s,\"ssid\":\"%s\",\"address\":\"192.168.10.1\",\"url\":\"http://192.168.10.1:%u/\",\"start_after_seconds\":45,\"error\":%d%s%s%s}",s->ready?"true":"false",camera_wifi_recovery_active(s)?"true":"false",s->ssid,CAMERA_HTTP_PORT,s->error,include_password?",\"password\":\"":"",include_password?s->password:"",include_password?"\"":"");
    return n<0||(size_t)n>=cap?-ENOSPC:0;
}
#ifdef CAMERA_WIFI_RECOVERY_PROBE
int main(int argc,char **argv){int fd=open("/etc/conf/camera",O_RDONLY|O_DIRECTORY);struct camera_wifi_recovery *s=NULL;int rc=camera_wifi_recovery_open(&s,fd);close(fd);if(rc)return 1;camera_wifi_recovery_request(s,1);unsigned channel=argc>1?(unsigned)atoi(argv[1]):6;for(unsigned i=0;i<180;i++){camera_wifi_recovery_tick(s,(uint64_t)i*1000+1,0,channel,0);if(i%10==0){char json[512];camera_wifi_recovery_json(s,json,sizeof json,0);puts(json);fflush(stdout);}sleep(1);}camera_wifi_recovery_stop(s);return 0;}
#endif
