#define _GNU_SOURCE
/* Small SNTP client (RFC4330). Never touches storage other than a RAM marker. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef CAMERA_IMAGE_BUILD_EPOCH
#define CAMERA_IMAGE_BUILD_EPOCH 0
#endif
/* Pure policy: a build floor prevents a bogus old RTC date, not proof of the
 * actual current time. SNTP still has to establish synchronized UTC. */
static int floor_needed(int64_t now,int64_t floor)
{return floor>0&&now<floor;}
static void initialize_floor(void)
{
    struct timespec current;
    if(clock_gettime(CLOCK_REALTIME,&current))return;
    int64_t epoch=(int64_t)CAMERA_IMAGE_BUILD_EPOCH;
    if(!floor_needed((int64_t)current.tv_sec,epoch))return;
    struct timespec floor={.tv_sec=(time_t)epoch,.tv_nsec=0};
    if((int64_t)floor.tv_sec!=epoch){fprintf(stderr,"CAMERA_CLOCK floor_failed=unsupported_time_range accuracy=unsynchronized\n");return;}
    if(clock_settime(CLOCK_REALTIME,&floor)){fprintf(stderr,"CAMERA_CLOCK floor_failed=%s accuracy=unsynchronized\n",strerror(errno));return;}
    printf("CAMERA_CLOCK source=image-build-floor accuracy=unsynchronized unix=%lld\n",(long long)epoch);fflush(stdout);
}

#define NTP_EPOCH UINT64_C(2208988800)
static uint32_t word(const unsigned char *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t monotonic_ns(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static uint64_t timestamp(const unsigned char *p)
{
    /* Unfold the 2036 NTP era; accept only 2020..2099 below. */
    uint64_t seconds=word(p);if(seconds<NTP_EPOCH)seconds+=UINT64_C(1)<<32;
    return (seconds-NTP_EPOCH)*1000000000u+((uint64_t)word(p+4)*1000000000u>>32);
}
static int reply(const unsigned char *request,const unsigned char *data,size_t size,
                 uint64_t elapsed,struct timespec *result)
{
    if(size!=48)return -1;
    unsigned version=(data[0]>>3)&7;
    if((data[0]>>6)==3||(data[0]&7)!=4||(version!=3&&version!=4)||
       !data[1]||data[1]>15||memcmp(request+40,data+24,8)||elapsed>2000000000u)return -1;
    uint64_t received=timestamp(data+32),sent=timestamp(data+40);
    if(received<UINT64_C(1577836800000000000)||sent>=UINT64_C(4102444800000000000)||
       sent<received||sent-received>elapsed)return -1;
    uint64_t now=sent+(elapsed-(sent-received))/2;
    result->tv_sec=(time_t)(now/1000000000u);result->tv_nsec=(long)(now%1000000000u);return 0;
}
static int synchronize(const char *server,const char *port,int probe)
{
    struct addrinfo hints={0},*addresses=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_DGRAM;
    if(getaddrinfo(server,port,&hints,&addresses))return -1;
    int rc=-1;
    for(struct addrinfo *a=addresses;a;a=a->ai_next){
        int fd=socket(a->ai_family,SOCK_DGRAM|SOCK_CLOEXEC,a->ai_protocol);if(fd<0)continue;
        unsigned char request[48]={0},response[49]={0};request[0]=0x23;
        struct timespec wall;clock_gettime(CLOCK_REALTIME,&wall);
        uint32_t seconds=htonl((uint32_t)((uint64_t)wall.tv_sec+NTP_EPOCH));memcpy(request+40,&seconds,4);
        if(getrandom(request+44,4,GRND_NONBLOCK)!=4){
            /* This is an unauthenticated SNTP request timestamp, not a key or
             * session token. It must also work before the crypto pool seeds. */
            int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
            ssize_t count=random<0?-1:read(random,request+44,4);if(random>=0)close(random);
            if(count!=4){close(fd);continue;}
        }
        if(connect(fd,a->ai_addr,a->ai_addrlen)){close(fd);continue;}
        uint64_t start=monotonic_ns();struct pollfd wait={.fd=fd,.events=POLLIN};
        if(send(fd,request,sizeof request,0)==sizeof request&&poll(&wait,1,1800)>0){
            ssize_t size=recv(fd,response,sizeof response,MSG_DONTWAIT);uint64_t end=monotonic_ns();
            if(size>0&&!reply(request,response,(size_t)size,end-start,&wall)&&
               (probe||!clock_settime(CLOCK_REALTIME,&wall))){
                printf("CAMERA_CLOCK source=SNTP synchronized=yes unix=%lld rtt_us=%llu\n",
                    (long long)wall.tv_sec,(unsigned long long)((end-start)/1000));fflush(stdout);rc=0;
            }
        }
        close(fd);if(!rc)break;
    }
    freeaddrinfo(addresses);return rc;
}
#ifndef CAMERA_TIME_TEST
int main(int argc,char **argv)
{
    /* Probe mode never adjusts time and is suitable for host/fixture checks. */
    if(argc>=3&&!strcmp(argv[1],"--probe"))return synchronize(argv[2],argc==4?argv[3]:"123",1)?1:0;
    if(argc!=1)return 2;
    initialize_floor();
    /* DHCP creates resolv.conf. Avoid backing off before Wi-Fi even has DNS. */
    for(unsigned i=0;i<12&&access("/etc/resolv.conf",R_OK);i++)sleep(1);
    const char *servers[]={"time.cloudflare.com","time.google.com"};unsigned failed=0;
    for(;;){
        /* Many LAN routers provide NTP even when Internet access is blocked. */
        char gateway[64]={0};struct in_addr address;FILE *source=fopen("/var/run/camera-time-gateway","r");
        if(source){if(!fgets(gateway,sizeof gateway,source))gateway[0]=0;fclose(source);gateway[strcspn(gateway,"\r\n")]=0;}
        int ok=inet_pton(AF_INET,gateway,&address)==1&&!synchronize(gateway,"123",0);
        for(unsigned i=0;!ok&&i<sizeof servers/sizeof servers[0];i++)if(!synchronize(servers[i],"123",0))ok=1;
        if(ok){FILE *marker=fopen("/var/run/camera-time-synchronized","w");if(marker){fputs("SNTP\n",marker);fclose(marker);}failed=0;sleep(3600);}
        else {sleep(16u<<failed);if(failed<5)failed++;}
    }
}
#endif
