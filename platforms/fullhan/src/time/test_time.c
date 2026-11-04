#define CAMERA_TIME_TEST
#include "camera_time.c"
#include <assert.h>
static void stamp(unsigned char *p,uint64_t seconds)
{uint32_t n=htonl((uint32_t)(seconds+NTP_EPOCH));memcpy(p,&n,4);memset(p+4,0,4);}
int main(void)
{
    unsigned char request[48]={0},response[48]={0};struct timespec result;
    request[40]=1;request[47]=42;response[0]=0x24;response[1]=2;
    memcpy(response+24,request+40,8);stamp(response+32,1793212000);stamp(response+40,1793212000);
    assert(!reply(request,response,48,100000000,&result));assert(result.tv_sec==1793212000&&result.tv_nsec==50000000);
    assert(reply(request,response,47,100000000,&result));
    response[24]^=1;assert(reply(request,response,48,100000000,&result));response[24]^=1;
    response[0]|=0xc0;assert(reply(request,response,48,100000000,&result));response[0]=0x24;
    response[1]=0;assert(reply(request,response,48,100000000,&result));response[1]=2;
    assert(reply(request,response,48,3000000000u,&result));
    stamp(response+40,1793212002);assert(reply(request,response,48,100000000,&result));
    stamp(response+32,2208988800);stamp(response+40,2208988800);assert(!reply(request,response,48,100000000,&result));assert(result.tv_sec==2208988800);
    stamp(response+32,100);stamp(response+40,100);assert(reply(request,response,48,100000000,&result));
    /* Exercise the network function without packets or clock changes. */
    assert(synchronize("invalid.invalid", "123",1));
    puts("PASS SNTP: current/2036-era clocks, truncation, origin, leap, KoD, delay and timestamp bounds");
}
