#define _GNU_SOURCE
#include "http.h"
#include "credentials.h"
#ifdef CAMERA_CONTROL_UI_HEADER
#include CAMERA_CONTROL_UI_HEADER
#else
#include "ui.h"
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEADER_CAP 8192u
#define FORM_CAP 4096u
#define UPDATE_CAP (8u*1024u*1024u)
#ifndef CONTROL_IDLE_MS
#define CONTROL_IDLE_MS 5000u
#endif
#ifndef CONTROL_TOTAL_MS
#define CONTROL_TOTAL_MS 90000u
#endif
struct connection {
    int fd,error;
    uint64_t start,progress,remaining;
    char input[HEADER_CAP+1];
    size_t prefetched,position;
};
struct service { struct control_http_config config;int listener;char authorization[1024]; };
static uint64_t milliseconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u;}
static int wait_socket(struct connection *c,short events)
{
    for(;;) {
        uint64_t now=milliseconds(),idle=now-c->progress,total=now-c->start;
        if(idle>=CONTROL_IDLE_MS||total>=CONTROL_TOTAL_MS){errno=ETIMEDOUT;c->error=408;return -1;}
        unsigned left=(unsigned)(CONTROL_IDLE_MS-idle),limit=(unsigned)(CONTROL_TOTAL_MS-total);
        struct pollfd p={c->fd,events,0};
        int rc=poll(&p,1,(int)(left<limit?left:limit));
        if(rc<0&&errno==EINTR)continue;
        if(rc<0){c->error=400;return -1;}
        if(!rc)continue;
        if(p.revents&events)return 0;
        errno=ECONNRESET;c->error=400;return -1;
    }
}
static int send_all(struct connection *c,const void *buffer,size_t bytes)
{
    const char *p=buffer;
    while(bytes) {
        if(wait_socket(c,POLLOUT))return -1;
        ssize_t n=send(c->fd,p,bytes,MSG_NOSIGNAL);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))continue;
        if(n<=0)return -1;
        p+=n;bytes-=(size_t)n;c->progress=milliseconds();
    }
    return 0;
}
static ssize_t body_read(void *opaque,void *buffer,size_t capacity)
{
    struct connection *c=opaque;
    if(!capacity||!c->remaining)return 0;
    if(c->error)return -1;
    if(capacity>c->remaining)capacity=(size_t)c->remaining;
    if(c->position<c->prefetched) {
        size_t count=c->prefetched-c->position;if(count>capacity)count=capacity;
        memcpy(buffer,c->input+c->position,count);c->position+=count;c->remaining-=count;
        return(ssize_t)count;
    }
    for(;;) {
        if(wait_socket(c,POLLIN))return -1;
        ssize_t n=recv(c->fd,buffer,capacity,0);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))continue;
        if(n<=0){c->error=400;if(!n)errno=ECONNRESET;return -1;}
        c->remaining-=(size_t)n;c->progress=milliseconds();return n;
    }
}
static int hex(unsigned char c)
{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int decode(const char *p,size_t bytes,char *out,size_t capacity)
{
    size_t used=0;
    for(size_t i=0;i<bytes;i++) {
        unsigned char c=(unsigned char)p[i];
        if(c=='%') {
            if(bytes-i<3)return -1;
            int a=hex((unsigned char)p[++i]),b=hex((unsigned char)p[++i]);if(a<0||b<0)return -1;
            c=(unsigned char)(a*16+b);
        } else if(c=='+')c=' ';
        if(!c||used+1>=capacity)return -1;
        out[used++]=(char)c;
    }
    if(!capacity)return -1;
    out[used]=0;return 0;
}
int control_form(const char *form,const char *wanted,char *out,size_t capacity)
{
    if(!form||!wanted||!out||!capacity)return -1;
    int found=0;out[0]=0;
    for(const char *p=form;*p;) {
        const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        const char *equals=memchr(p,'=',(size_t)(end-p));if(!equals)return -1;
        char name[128],value[FORM_CAP+1];
        if(decode(p,(size_t)(equals-p),name,sizeof name)||!name[0]||
           decode(equals+1,(size_t)(end-equals-1),value,sizeof value))return -1;
        if(!strcmp(name,wanted)) {
            if(found||strlen(value)>=capacity)return -1;
            strcpy(out,value);found=1;
        }
        if(!*end)break;
        p=end+1;if(!*p)return -1;
    }
    return found;
}
int control_body_text(const struct control_body *body,char *out,size_t capacity)
{
    if(!body||!out||!capacity||body->length>=capacity||(!body->read&&body->length))return -1;
    size_t used=0;
    while(used<body->length) {
        ssize_t n=body->read(body->opaque,out+used,(size_t)body->length-used);
        if(n<=0||(uint64_t)n>body->length-used)return -1;
        used+=(size_t)n;
    }
    if(memchr(out,0,used))return -1;
    out[used]=0;return 0;
}
int control_json_string(char *out,size_t capacity,const char *input)
{
    static const char digits[]="0123456789abcdef";
    if(!out||!input||capacity<3)return -1;
    size_t n=0;out[n++]='"';
    for(const unsigned char *p=(const unsigned char *)input;*p;p++) {
        unsigned c=*p;
        if(c=='"'||c=='\\'){if(n+2>=capacity)return -1;out[n++]='\\';out[n++]=(char)c;}
        else if(c<32||c=='<'||c=='>'||c=='&') {
            if(n+6>=capacity)return -1;
            out[n++]='\\';out[n++]='u';out[n++]='0';out[n++]='0';out[n++]=digits[c>>4];out[n++]=digits[c&15];
        } else {if(n+1>=capacity)return -1;out[n++]=(char)c;}
    }
    if(n+2>capacity)return -1;
    out[n++]='"';out[n]=0;return 0;
}
static int secret_equal(const char *a,const char *b)
{
    size_t n=strlen(a);if(n!=strlen(b))return 0;
    unsigned diff=0;for(size_t i=0;i<n;i++)diff|=(unsigned char)a[i]^(unsigned char)b[i];return !diff;
}
static const char *reason(unsigned status)
{
    switch(status){case 200:return"OK";case 202:return"Accepted";case 400:return"Bad Request";
    case 401:return"Unauthorized";case 403:return"Forbidden";case 404:return"Not Found";
    case 405:return"Method Not Allowed";case 408:return"Request Timeout";case 411:return"Length Required";
    case 413:return"Payload Too Large";case 415:return"Unsupported Media Type";case 417:return"Expectation Failed";
    case 431:return"Request Header Fields Too Large";case 503:return"Service Unavailable";default:return"Response";}
}
static void reply(struct connection *c,unsigned status,const char *type,const char *text)
{
    char header[1300];
    int n=snprintf(header,sizeof header,"HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; media-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'; object-src 'none'\r\n%s\r\n",
        status,reason(status),type,strlen(text),status==401?"WWW-Authenticate: Basic realm=\"Camera control\", charset=\"UTF-8\"\r\n":"");
    if(n>0&&(size_t)n<sizeof header&&!send_all(c,header,(size_t)n))send_all(c,text,strlen(text));
}
static void error_reply(struct connection *c,unsigned status,const char *message)
{
    char escaped[256],text[300];
    if(control_json_string(escaped,sizeof escaped,message))strcpy(escaped,"\"Request failed\"");
    snprintf(text,sizeof text,"{\"error\":%s}",escaped);reply(c,status,"application/json; charset=utf-8",text);
}
static int length_value(const char *p,uint64_t *out)
{
    if(!*p)return -1;
    uint64_t n=0;
    for(;*p;p++){if(*p<'0'||*p>'9'||n>(UINT64_MAX-(unsigned)(*p-'0'))/10)return -1;n=n*10+(unsigned)(*p-'0');}
    *out=n;return 0;
}
static int content_type(const char *actual,const char *expected)
{
    size_t n=strlen(expected);if(strncasecmp(actual,expected,n))return 0;
    return !actual[n]||actual[n]==';';
}
static int handle(struct service *s,int fd)
{
    struct connection c={.fd=fd,.start=milliseconds()};c.progress=c.start;
    size_t bytes=0,end=0;
    while(!end) {
        if(bytes==HEADER_CAP){error_reply(&c,431,"Request headers too large");return 0;}
        if(wait_socket(&c,POLLIN))return 0;
        ssize_t n=recv(fd,c.input+bytes,HEADER_CAP-bytes,0);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0)return 0;
        bytes+=(size_t)n;c.progress=milliseconds();
        for(size_t i=0;i+3<bytes;i++)if(!memcmp(c.input+i,"\r\n\r\n",4)){end=i+4;break;}
    }
    if(memchr(c.input,0,end)){error_reply(&c,400,"Malformed headers");return 0;}
    c.input[bytes]=0;
    char *line_end=strstr(c.input,"\r\n");*line_end=0;
    char *method=c.input,*path=strchr(method,' ');if(!path){error_reply(&c,400,"Malformed request");return 0;}
    *path++=0;char *version=strchr(path,' ');if(!version){error_reply(&c,400,"Malformed request");return 0;}*version++=0;
    if((strcmp(version,"HTTP/1.1")&&strcmp(version,"HTTP/1.0"))||path[0]!='/'||strlen(path)>256||
       strchr(path,' ')||strchr(path,'\t')||strchr(path,'#')){error_reply(&c,400,"Malformed request");return 0;}
    for(const unsigned char *p=(const unsigned char *)path;*p;p++)if(*p<33||*p>126){error_reply(&c,400,"Malformed request");return 0;}
    const char *names[64],*host=NULL,*origin=NULL,*authorization=NULL,*type="",*csrf=NULL;
    unsigned count=0;int has_length=0;
    uint64_t length=0;
    for(char *p=line_end+2;p<c.input+end-2;) {
        char *next=strstr(p,"\r\n");if(!next){error_reply(&c,400,"Malformed headers");return 0;}*next=0;
        char *colon=strchr(p,':');if(!colon||colon==p){error_reply(&c,400,"Malformed headers");return 0;}
        *colon=0;
        for(const unsigned char *q=(const unsigned char *)p;*q;q++)if(!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='-')){error_reply(&c,400,"Malformed header name");return 0;}
        if(count==64){error_reply(&c,431,"Too many headers");return 0;}
        for(unsigned i=0;i<count;i++)if(!strcasecmp(names[i],p)){error_reply(&c,400,"Duplicate header");return 0;}
        names[count++]=p;char *value=colon+1;while(*value==' '||*value=='\t')value++;
        char *tail=next;while(tail>value&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
        for(const unsigned char *q=(const unsigned char *)value;*q;q++)if((*q<32&&*q!='\t')||*q==127){error_reply(&c,400,"Malformed header value");return 0;}
        if(!strcasecmp(p,"Host"))host=value;
        else if(!strcasecmp(p,"Origin"))origin=value;
        else if(!strcasecmp(p,"Authorization"))authorization=value;
        else if(!strcasecmp(p,"Content-Type"))type=value;
        else if(!strcasecmp(p,"X-Camera-Request"))csrf=value;
        else if(!strcasecmp(p,"Content-Length")){if(length_value(value,&length)){error_reply(&c,400,"Invalid content length");return 0;}has_length=1;}
        else if(!strcasecmp(p,"Transfer-Encoding")){error_reply(&c,400,"Transfer encoding is not supported");return 0;}
        else if(!strcasecmp(p,"Expect")){error_reply(&c,417,"Expect is not supported");return 0;}
        p=next+2;
    }
    if(!host||!*host||strchr(host,'/')||strchr(host,' ')||strchr(host,'\t')||strchr(host,'@')||strchr(host,'\\')){error_reply(&c,400,"Invalid Host");return 0;}
    char current_auth[1024];const char *expected=s->authorization;
    if(camera_credentials_token){int rc=camera_credentials_token(0,current_auth,sizeof current_auth,NULL);if(rc<0){error_reply(&c,503,"Authentication unavailable");return 0;}if(rc>0)expected=current_auth;}
    if(!authorization||!secret_equal(authorization,expected)){error_reply(&c,401,"Authentication required");return 0;}
    int post=!strcmp(method,"POST"),get=!strcmp(method,"GET");
    if(!post&&!get){error_reply(&c,405,"Use GET or POST");return 0;}
    if(post) {
        if(!csrf||strcmp(csrf,"1")){error_reply(&c,403,"Missing request protection");return 0;}
        if(origin&&(strncasecmp(origin,"http://",7)||strcasecmp(origin+7,host))){error_reply(&c,403,"Origin does not match this camera");return 0;}
        if(!has_length){error_reply(&c,411,"Content-Length is required");return 0;}
        int update=!strcmp(path,"/api/update");
        if(length>(update?UPDATE_CAP:FORM_CAP)){error_reply(&c,413,"Request body too large");return 0;}
        if(length&&!content_type(type,update?"application/octet-stream":"application/x-www-form-urlencoded")){error_reply(&c,415,"Unsupported Content-Type");return 0;}
    } else if(length){error_reply(&c,400,"GET body is not supported");return 0;}
    if(get&&!strcmp(path,"/")){reply(&c,200,"text/html; charset=utf-8",control_ui_html);return 0;}
    if(get&&!strcmp(path,"/app.js")){reply(&c,200,"text/javascript; charset=utf-8",control_ui_js);return 0;}
    if(get&&!strcmp(path,"/style.css")){reply(&c,200,"text/css; charset=utf-8",control_ui_css);return 0;}
    if(get&&!strcmp(path,"/api/board")){reply(&c,200,"application/json",CAMERA_BOARD_JSON);return 0;}
    if((!CAMERA_HAS_IRCUT&&!CAMERA_HAS_IR_LED&&!strcmp(path,"/api/illumination"))||(!CAMERA_HAS_PTZ&&!strncmp(path,"/api/ptz",8))||(!CAMERA_HAS_ROTATION&&!strcmp(path,"/api/rotation"))||(!CAMERA_HAS_PICTURE&&!strncmp(path,"/api/picture",12))||(!CAMERA_HAS_STATUS_LED&&!strcmp(path,"/api/indicator"))){error_reply(&c,404,"Feature disabled for this board");return 0;}
    if(get&&!strcmp(path,"/api/preview.mp4")){
        if(s->config.preview&&!s->config.preview(s->config.opaque,fd))return 1;
        error_reply(&c,503,"Preview unavailable or all preview connections are in use");return 0;
    }
    if(strncmp(path,"/api/",5)){error_reply(&c,404,"Not found");return 0;}
    c.position=end;c.prefetched=bytes;c.remaining=length;
    struct control_request request={method,path,type,{&c,length,body_read}};
    struct control_reply result={.status=500,.json="{\"error\":\"No response\"}"};
    s->config.backend(s->config.opaque,&request,&result);
    /* Backend verification is CPU work, not a stalled network writer. Give
     * response I/O a fresh idle window; c.start still bounds the entire
     * request, including backend work, by CONTROL_TOTAL_MS. */
    c.progress=milliseconds();
    result.json[sizeof result.json-1]=0;
    if(c.error){error_reply(&c,(unsigned)c.error,c.error==408?"Request body timed out":"Request body was truncated");return 0;}
    if(c.remaining&&result.status<400){error_reply(&c,400,"Request body was not consumed");return 0;}
    if(result.status<200||result.status>599){error_reply(&c,500,"Invalid backend response");return 0;}
    reply(&c,result.status,"application/json; charset=utf-8",result.json);
    return 0;
}
static int prepare(struct service *s,const struct control_http_config *config)
{
    if(!config||!config->bind_address||!config->credentials||!config->backend||!config->port||config->port>65535)return -EINVAL;
    const char *credentials=config->credentials,*colon=strchr(credentials,':');
    size_t length=strlen(credentials);if(!colon||colon==credentials||!colon[1]||length>700||strchr(credentials,'\r')||strchr(credentials,'\n'))return -EINVAL;
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    strcpy(s->authorization,"Basic ");size_t k=6;
    for(size_t i=0;i<length;i+=3) {
        unsigned v=(unsigned char)credentials[i]<<16;
        if(i+1<length)v|=(unsigned char)credentials[i+1]<<8;
        if(i+2<length)v|=(unsigned char)credentials[i+2];
        s->authorization[k++]=alphabet[v>>18];s->authorization[k++]=alphabet[v>>12&63];
        s->authorization[k++]=i+1<length?alphabet[v>>6&63]:'=';s->authorization[k++]=i+2<length?alphabet[v&63]:'=';
    }
    s->authorization[k]=0;s->config=*config;
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((uint16_t)config->port);
    if(inet_pton(AF_INET,config->bind_address,&address.sin_addr)!=1)return -EINVAL;
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    int reuse=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    if(bind(fd,(struct sockaddr *)&address,sizeof address)||listen(fd,4)){int error=errno;close(fd);return -error;}
    s->listener=fd;return 0;
}
static void finish_connection(int fd)
{
    /* Closing with unread request bytes can reset TCP and discard the JSON
     * response (notably an early signature rejection). Send FIN first, then
     * give small/in-flight rejected bodies a strictly bounded drain window.
     * Incoming bytes never extend this deadline or the 256KiB drain cap. */
    if(shutdown(fd,SHUT_WR)) {close(fd);return;}
    uint64_t deadline=milliseconds()+150u;
    size_t drained=0;char buffer[4096];
    while(drained<256u*1024u) {
        uint64_t now=milliseconds();if(now>=deadline)break;
        struct pollfd p={fd,POLLIN,0};int rc=poll(&p,1,(int)(deadline-now));
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)break;
        ssize_t n=recv(fd,buffer,sizeof buffer,0);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0)break;
        drained+=(size_t)n;
    }
    close(fd);
}
static int serve(struct service *s)
{
    for(;;) {
        struct pollfd p={s->listener,POLLIN,0};int rc=poll(&p,1,-1);
        if(rc<0&&errno==EINTR)continue;
        if(rc<0)return -errno;
        int fd=accept4(s->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(fd<0){if(errno==EAGAIN||errno==EINTR)continue;return -errno;}
        if(!handle(s,fd))finish_connection(fd);
    }
}
int control_http_run(const struct control_http_config *config)
{
    struct service service={0};int rc=prepare(&service,config);if(rc)return rc;
    rc=serve(&service);close(service.listener);return rc;
}
static void *worker(void *opaque)
{struct service *s=opaque;serve(s);close(s->listener);free(s);return NULL;}
int control_http_start(const struct control_http_config *config)
{
    struct service *s=calloc(1,sizeof *s);if(!s)return -ENOMEM;
    int rc=prepare(s,config);if(rc){free(s);return rc;}
    pthread_attr_t attr;rc=pthread_attr_init(&attr);
    if(rc){close(s->listener);free(s);return -rc;}
    rc=pthread_attr_setstacksize(&attr,128u*1024u);
    if(!rc)rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    pthread_t thread;if(!rc)rc=pthread_create(&thread,&attr,worker,s);
    pthread_attr_destroy(&attr);if(rc){close(s->listener);free(s);return -rc;}
    return 0;
}
