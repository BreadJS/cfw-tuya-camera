#define CAMERA_TIME_TEST
#include "../../src/time/camera_time.c"
#include <assert.h>
static void ntp(unsigned char out[8],uint32_t unix_time)
{uint32_t seconds=htonl(unix_time+(uint32_t)NTP_EPOCH);memcpy(out,&seconds,4);memset(out+4,0,4);}
int main(void)
{
    (void)synchronize;(void)initialize_floor;
    assert(floor_needed(946684800,1793278800));assert(!floor_needed(1793278800,1793278800));assert(!floor_needed(1793278801,1793278800));assert(!floor_needed(946684800,0));
    unsigned char request[48]={0},response[48]={0};
    memcpy(request+40,"12345678",8);memcpy(response+24,request+40,8);response[0]=0x24;response[1]=2;
    ntp(response+32,1793278800u);ntp(response+40,1793278800u);
    struct timespec result;
    assert(!reply(request,response,sizeof response,100000000,&result));assert(result.tv_sec==1793278800&&result.tv_nsec==50000000);
    setenv("TZ","Europe/Amsterdam",1);tzset();assert(!reply(request,response,48,100000000,&result)&&result.tv_sec==1793278800);
    response[24]^=1;assert(reply(request,response,48,100000000,&result)<0);response[24]^=1;
    response[0]=0xe4;assert(reply(request,response,48,100000000,&result)<0);response[0]=0x24;
    response[1]=0;assert(reply(request,response,48,100000000,&result)<0);response[1]=2;
    assert(reply(request,response,47,100000000,&result)<0);assert(reply(request,response,48,2000000001u,&result)<0);
    ntp(response+32,1793278801u);assert(reply(request,response,48,100000000,&result)<0);
    ntp(response+32,1577836799u);ntp(response+40,1577836799u);assert(reply(request,response,48,100000000,&result)<0);
    puts("PASS camera time: UTC epoch independent of timezone, bounded SNTP roundtrip, nonce/leap/stratum/date validation");
}
