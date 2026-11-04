#define _GNU_SOURCE
#include "wifi_radio.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
struct packet {struct nlmsghdr n;struct genlmsghdr g;unsigned char data[512];};
static void attribute(struct packet *p,unsigned type,const void *data,size_t size)
{
    struct nlattr *a=(void *)((unsigned char *)p+p->n.nlmsg_len);
    a->nla_type=(uint16_t)type;a->nla_len=(uint16_t)(NLA_HDRLEN+size);
    memcpy((char *)a+NLA_HDRLEN,data,size);p->n.nlmsg_len+=(unsigned)NLA_ALIGN(a->nla_len);
}
static int exchange(int fd,struct packet *p,int family)
{
    struct sockaddr_nl peer={.nl_family=AF_NETLINK};
    if(sendto(fd,p,p->n.nlmsg_len,0,(struct sockaddr *)&peer,sizeof peer)!=(ssize_t)p->n.nlmsg_len)return -errno;
    unsigned char buffer[8192];
    for(unsigned attempt=0;attempt<4;attempt++) {
        struct pollfd poller={fd,POLLIN,0};int ready;
        do{ready=poll(&poller,1,1000);}while(ready<0&&errno==EINTR);
        if(ready<=0)return ready<0?-errno:-ETIMEDOUT;
        ssize_t bytes=recv(fd,buffer,sizeof buffer,MSG_TRUNC);if(bytes<0)return -errno;if(bytes>(ssize_t)sizeof buffer)return -EOVERFLOW;
        for(struct nlmsghdr *n=(void *)buffer;NLMSG_OK(n,bytes);n=NLMSG_NEXT(n,bytes)) {
            if(n->nlmsg_seq!=p->n.nlmsg_seq)continue;
            if(n->nlmsg_type==NLMSG_ERROR){if(n->nlmsg_len<NLMSG_LENGTH(sizeof(struct nlmsgerr)))return -EPROTO;return ((struct nlmsgerr *)NLMSG_DATA(n))->error;}
            if(family&&n->nlmsg_type==GENL_ID_CTRL) {
                size_t left=n->nlmsg_len-NLMSG_HDRLEN-GENL_HDRLEN;
                for(struct nlattr *a=(void *)((char *)NLMSG_DATA(n)+GENL_HDRLEN);left>=NLA_HDRLEN&&a->nla_len>=NLA_HDRLEN&&a->nla_len<=left;) {
                    if(a->nla_type==CTRL_ATTR_FAMILY_ID&&a->nla_len==NLA_HDRLEN+2){uint16_t id;memcpy(&id,(char *)a+NLA_HDRLEN,2);return id;}
                    size_t step=NLA_ALIGN(a->nla_len);if(step>left)break;left-=step;a=(void *)((char *)a+step);
                }
            }
        }
    }return -EPROTO;
}
static int interface(unsigned command)
{
    unsigned index=if_nametoindex(command==NL80211_CMD_NEW_INTERFACE?"wlan0":"cameraap0");if(!index)return -ENODEV;
    if(command==NL80211_CMD_NEW_INTERFACE&&if_nametoindex("cameraap0"))return -EEXIST;
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_GENERIC);if(fd<0)return -errno;
    struct sockaddr_nl local={.nl_family=AF_NETLINK};if(bind(fd,(struct sockaddr *)&local,sizeof local)){int rc=-errno;close(fd);return rc;}
    struct packet p={.n={.nlmsg_len=NLMSG_HDRLEN+GENL_HDRLEN,.nlmsg_type=GENL_ID_CTRL,.nlmsg_flags=NLM_F_REQUEST,.nlmsg_seq=1},.g={.cmd=CTRL_CMD_GETFAMILY,.version=1}};
    attribute(&p,CTRL_ATTR_FAMILY_NAME,"nl80211",8);int family=exchange(fd,&p,1);
    if(family>0){memset(&p,0,sizeof p);p.n=(struct nlmsghdr){.nlmsg_len=NLMSG_HDRLEN+GENL_HDRLEN,.nlmsg_type=(uint16_t)family,.nlmsg_flags=NLM_F_REQUEST|NLM_F_ACK,.nlmsg_seq=2};p.g.cmd=(uint8_t)command;p.g.version=1;
        attribute(&p,NL80211_ATTR_IFINDEX,&index,sizeof index);
        if(command==NL80211_CMD_NEW_INTERFACE){unsigned type=NL80211_IFTYPE_AP;attribute(&p,NL80211_ATTR_IFNAME,"cameraap0",10);attribute(&p,NL80211_ATTR_IFTYPE,&type,sizeof type);}
        family=exchange(fd,&p,0);
    }close(fd);return family;
}
int camera_wifi_ap_create(void){return interface(NL80211_CMD_NEW_INTERFACE);}
int camera_wifi_ap_delete(void){return interface(NL80211_CMD_DEL_INTERFACE);}
int camera_wifi_ap_address(void)
{
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    struct ifreq request={0};strcpy(request.ifr_name,"cameraap0");int rc=0;
    struct sockaddr_in *address=(void *)&request.ifr_addr;address->sin_family=AF_INET;inet_pton(AF_INET,"192.168.10.1",&address->sin_addr);
    if(ioctl(fd,SIOCSIFADDR,&request))rc=-errno;
    if(!rc){inet_pton(AF_INET,"255.255.255.0",&address->sin_addr);if(ioctl(fd,SIOCSIFNETMASK,&request))rc=-errno;}
    if(!rc&&ioctl(fd,SIOCGIFFLAGS,&request))rc=-errno;
    if(!rc){request.ifr_flags|=IFF_UP;if(ioctl(fd,SIOCSIFFLAGS,&request))rc=-errno;}
    close(fd);return rc;
}
int camera_wifi_station_ready(void)
{
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 0;
    struct ifreq request={0};strcpy(request.ifr_name,"wlan0");int ready=0;
    if(!ioctl(fd,SIOCGIFADDR,&request)){struct sockaddr_in *a=(void *)&request.ifr_addr;uint32_t ip=ntohl(a->sin_addr.s_addr);ready=ip&&ip!=0xc0a80a01U&&(ip>>16)!=0xa9feU;}
    close(fd);return ready;
}
#ifdef CAMERA_WIFI_RADIO_PROBE
int main(int argc,char **argv){int rc=argc>1&&!strcmp(argv[1],"delete")?camera_wifi_ap_delete():camera_wifi_ap_create();printf("AP_INTERFACE_RESULT rc=%d station_address=%d\n",rc,camera_wifi_station_ready());return rc?1:0;}
#endif
