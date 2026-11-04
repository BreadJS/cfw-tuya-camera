/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "wifi.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
struct anyka_network { char config[256],runtime[256]; pthread_mutex_t lock; };
static int path(char *out,size_t n,const char *dir,const char *name) {
    int r=snprintf(out,n,"%s/%s",dir,name); return r<0||(size_t)r>=n?-1:0;
}
static int readfile(const char *dir,const char *name,char *out,size_t n) {
    char p[512]; if(path(p,sizeof p,dir,name))return -1;
    int f=open(p,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); if(f<0)return -1;
    ssize_t r=read(f,out,n-1); close(f); if(r<0)return -1; out[r]=0;return 0;
}
static int writefile(const char *dir,const char *name,const char *s) {
    char p[512],tmp[512];if(path(p,sizeof p,dir,name)||path(tmp,sizeof tmp,dir,".http-write-XXXXXX"))return -1;
    int f=mkostemp(tmp,O_CLOEXEC);if(f<0)return -1;
    size_t n=strlen(s);ssize_t r=write(f,s,n);int e=(r!=(ssize_t)n||fsync(f));close(f);
    if(!e)e=rename(tmp,p);
    if(!e){int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(d<0)e=1;else{e=fsync(d);close(d);}}
    unlink(tmp);return e?-1:0;
}
static void removefile(const char *dir,const char *name) {char p[512];if(!path(p,sizeof p,dir,name))unlink(p);}
static unsigned long uptime(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long)t.tv_sec;}
static int control(struct anyka_network *n,const char *cmd,char *out,size_t cap) {
    struct sockaddr_un local={.sun_family=AF_UNIX},remote={.sun_family=AF_UNIX};
    if(path(remote.sun_path,sizeof remote.sun_path,n->runtime,"wpa_ctrl/wlan0"))return -1;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
    snprintf(local.sun_path,sizeof local.sun_path,"%.70s/http-%ld",n->runtime,(long)getpid());
    unlink(local.sun_path);struct timeval timeout={.tv_sec=1};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    int ok=bind(fd,(void *)&local,sizeof local)||connect(fd,(void *)&remote,sizeof remote);
    if(!ok&&send(fd,cmd,strlen(cmd),0)!=(ssize_t)strlen(cmd))ok=-1;
    ssize_t r=ok?-1:recv(fd,out,cap-1,0);close(fd);unlink(local.sun_path);
    if(r<0)return -1;
    out[r]=0;return 0;
}
static int hexval(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static int tokenvalid(const char *s){if(strlen(s)!=32)return 0;for(unsigned i=0;i<32;i++)if(hexval(s[i])<0)return 0;return 1;}
static int ssidof(const char *config,char *ssid) {
    const char *s=strstr(config,"#camera_ssid_hex="); if(!s){ssid[0]=0;return -1;}s+=17;
    unsigned i=0;while(*s&&*s!='\n'&&i<32){int a=hexval(s[0]),b=hexval(s[1]);if(a<0||b<0)return -1;ssid[i++]=(char)(a*16+b);s+=2;}
    ssid[i]=0;return 0;
}
static void reply(struct control_reply *r,unsigned status,const char *s){r->status=status;snprintf(r->json,sizeof r->json,"%s",s);}
int anyka_network_open(struct anyka_network **out,const char *c,const char *r) {
    *out=NULL;if(!c)c="/etc/config/camera/wifi";if(!r)r="/var/run/camera-network";
    if(strlen(c)>240||strlen(r)>70)return -EINVAL;
    struct anyka_network *n=calloc(1,sizeof *n);if(!n)return -ENOMEM;
    strcpy(n->config,c);strcpy(n->runtime,r);
    if((mkdir(c,0700)&&errno!=EEXIST)||(mkdir(r,0700)&&errno!=EEXIST)){free(n);return -errno;}
    struct stat st; if(lstat(c,&st)||!S_ISDIR(st.st_mode)||(st.st_mode&0077)||lstat(r,&st)||!S_ISDIR(st.st_mode)||(st.st_mode&0077)){free(n);return -EPERM;}
    pthread_mutex_init(&n->lock,NULL);*out=n;return 0;
}
static void status(struct anyka_network *n,struct control_reply *r) {
    char buf[2048]="",ssid[33]="",quoted[200],id[80]="",deadline[40]="",wpa[2048]="";
    readfile(n->config,"current.conf",buf,sizeof buf);ssidof(buf,ssid);control_json_string(quoted,sizeof quoted,ssid);
    int pending=!readfile(n->runtime,"pending.id",id,sizeof id)&&tokenvalid(id);
    readfile(n->runtime,"pending.expires",deadline,sizeof deadline);unsigned long until=strtoul(deadline,NULL,10),now=uptime();
    int completed=!control(n,"STATUS",wpa,sizeof wpa)&&strstr(wpa,"wpa_state=COMPLETED\n");
    r->status=200;snprintf(r->json,sizeof r->json,"{\"ssid\":%s,\"completed\":%s,\"pending\":%s,\"transaction\":\"%s\",\"remaining_seconds\":%lu,\"confirmed\":%s}",quoted,completed?"true":"false",pending?"true":"false",pending?id:"",pending&&until>now?until-now:0,pending?"false":"true");
}
static void recovery(struct anyka_network *n,struct control_reply *r,int access) {
    char mode[80]="",ssid[80]="",q[250],error[180]="",qe[550],enabled[8]="";
    readfile(n->runtime,"mode",mode,sizeof mode);readfile(n->runtime,"ap.ssid",ssid,sizeof ssid);readfile(n->runtime,"error",error,sizeof error);readfile(n->config,"recovery-disabled",enabled,sizeof enabled);
    char supervisor[40]="";readfile(n->runtime,"supervisor.pid",supervisor,sizeof supervisor);
    char *end=NULL;long pid=strtol(supervisor,&end,10);int available=pid>1&&pid<2147483647&&end&&(*end==0||!strcmp(end,"\n"))&&!kill((pid_t)pid,0);
    control_json_string(q,sizeof q,ssid);control_json_string(qe,sizeof qe,error);
    r->status=200;snprintf(r->json,sizeof r->json,"{\"available\":%s,\"enabled\":%s,\"active\":%s,\"ready\":%s,\"ssid\":%s,\"error\":%s%s}",available?"true":"false",enabled[0]?"false":"true",!strcmp(mode,"ap")?"true":"false",!strcmp(mode,"ap")?"true":"false",q,qe,access?",\"password\":\"camera123\"":"");
}
static void scan(struct anyka_network *n,const struct control_request *req,struct control_reply *r) {
    char out[6144];int cached=0;
    if(!strcmp(req->method,"POST")&&(control(n,"SCAN",out,sizeof out)||strncmp(out,"OK",2)))cached=1;
    if(control(n,"SCAN_RESULTS",out,sizeof out)){cached=1;if(readfile(n->runtime,"scan-cache",out,sizeof out)){reply(r,200,"{\"scanning\":false,\"networks\":[],\"error\":\"No cached scan available in recovery AP mode\"}");return;}}
    size_t used=0;int count=0;used+=(size_t)snprintf(r->json,sizeof r->json,"{\"scanning\":%s,\"cached\":%s,\"networks\":[",!cached&&!strcmp(req->method,"POST")?"true":"false",cached?"true":"false");
    char *save=NULL,*line=strtok_r(out,"\n",&save);while((line=strtok_r(NULL,"\n",&save))&&count<24){
        char *fields[5];fields[0]=line;int k;for(k=1;k<5;k++){char *tab=strchr(fields[k-1],'\t');if(!tab)break;*tab=0;fields[k]=tab+1;}if(k!=5)continue;
        char ss[300];if(control_json_string(ss,sizeof ss,fields[4]))continue;
        const char *security=strstr(fields[3],"WPA")?"WPA/WPA2":strstr(fields[3],"WEP")?"WEP":"Open";
        int len=snprintf(r->json+used,sizeof r->json-used,"%s{\"ssid\":%s,\"frequency\":%d,\"signal_dbm\":%d,\"security\":\"%s\",\"hidden\":%s}",count?",":"",ss,atoi(fields[1]),atoi(fields[2]),security,fields[4][0]?"false":"true");
        if(len<0||(size_t)len>=sizeof r->json-used-4)break;
        used+=(size_t)len;count++;
    }snprintf(r->json+used,sizeof r->json-used,"]}");r->status=200;
}
void anyka_network_http(void *opaque,const struct control_request *req,struct control_reply *r) {
    struct anyka_network *n=opaque;if(!n){reply(r,503,"{\"error\":\"Network unavailable\"}");return;}
    pthread_mutex_lock(&n->lock);char form[1024]="";
    if(strcmp(req->method,"GET")&&strcmp(req->method,"POST")){reply(r,405,"{}");goto done;}
    if(!strcmp(req->method,"POST")&&control_body_text(&req->body,form,sizeof form)){reply(r,400,"{\"error\":\"Invalid form\"}");goto done;}
    if(!strcmp(req->path,"/api/wifi/scan")){scan(n,req,r);goto done;}
    if(!strcmp(req->path,"/api/wifi/recovery/access")){recovery(n,r,1);goto done;}
    if(!strcmp(req->path,"/api/wifi/recovery")){
        if(!strcmp(req->method,"POST")){char v[8];if(control_form(form,"enabled",v,sizeof v)!=1||(strcmp(v,"true")&&strcmp(v,"false"))){reply(r,400,"{}");goto done;}if(!strcmp(v,"false"))writefile(n->config,"recovery-disabled","1");else removefile(n->config,"recovery-disabled");writefile(n->runtime,"request",!strcmp(v,"true")?"recovery":"apply");}
        recovery(n,r,0);goto done;
    }
    if(!strcmp(req->path,"/api/wifi/confirm")||!strcmp(req->path,"/api/wifi/cancel")){
        char id[80],current[80],config[1024],wpa[2048],expires[40];
        if(strcmp(req->method,"POST")||control_form(form,"transaction",id,sizeof id)!=1||!tokenvalid(id)||readfile(n->runtime,"pending.id",current,sizeof current)||strcmp(id,current)){reply(r,409,"{\"error\":\"No matching pending change\"}");goto done;}
        if(strstr(req->path,"confirm")){
            if(readfile(n->runtime,"pending.expires",expires,sizeof expires)||strtoul(expires,NULL,10)<=uptime()||control(n,"STATUS",wpa,sizeof wpa)||!strstr(wpa,"wpa_state=COMPLETED\n")||readfile(n->runtime,"active.id",current,sizeof current)||strcmp(current,id)||readfile(n->runtime,"pending.conf",config,sizeof config)||writefile(n->config,"current.conf",config)){reply(r,409,"{\"error\":\"Candidate Wi-Fi is not connected\"}");goto done;}
        }
        removefile(n->runtime,"pending.id");writefile(n->runtime,"request","apply");status(n,r);goto done;
    }
    if(strcmp(req->path,"/api/wifi")&&strcmp(req->path,"/api/wifi/save")){reply(r,404,"{}");goto done;}
    if(!strcmp(req->method,"GET")){status(n,r);goto done;}
    char ssid[34],password[66],hex[65],escaped[130],config[1024],id[33],expires[40];
    if(control_form(form,"ssid",ssid,sizeof ssid)!=1||!ssid[0]||strlen(ssid)>32||control_form(form,"password",password,sizeof password)!=1||(password[0]&&(strlen(password)<8||strlen(password)>63))){reply(r,400,"{\"error\":\"SSID must be 1–32 bytes; WPA password 8–63 bytes or empty for open Wi-Fi\"}");goto done;}
    for(size_t i=0;i<strlen(ssid);i++)snprintf(hex+i*2,3,"%02x",(unsigned char)ssid[i]);
    size_t e=0;for(size_t i=0;i<strlen(password);i++){unsigned char c=(unsigned char)password[i];if(c<32||c>126){reply(r,400,"{\"error\":\"Password requires printable ASCII\"}");goto done;}if(c=='"'||c=='\\')escaped[e++]='\\';escaped[e++]=(char)c;}escaped[e]=0;
    unsigned char random[16];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);ssize_t got=fd<0?-1:read(fd,random,sizeof random);if(fd>=0)close(fd);if(got!=16){reply(r,500,"{}");goto done;}for(unsigned i=0;i<16;i++)snprintf(id+i*2,3,"%02x",random[i]);
    snprintf(config,sizeof config,"#camera_ssid_hex=%s\nctrl_interface=%s/wpa_ctrl\nupdate_config=0\nnetwork={\n ssid=%s\n scan_ssid=1\n%s%s%s\n}\n",hex,n->runtime,hex,password[0]?" key_mgmt=WPA-PSK\n psk=\"":" key_mgmt=NONE",password[0]?escaped:"",password[0]?"\"":"");
    snprintf(expires,sizeof expires,"%lu",uptime()+90);
    if(writefile(n->runtime,"pending.conf",config)||writefile(n->runtime,"pending.expires",expires)||writefile(n->runtime,"pending.id",id)||writefile(n->runtime,"request","apply")){reply(r,500,"{\"error\":\"Cannot stage Wi-Fi change\"}");goto done;}
    status(n,r);
done:pthread_mutex_unlock(&n->lock);
}
