#include "../../src/reset/button.h"
#include <assert.h>
#include <stdio.h>
static struct camera_reset_button s;
static unsigned events;
static uint64_t time_ms;
static void feed(int level,unsigned duration,int busy)
{
    for(unsigned i=0;i<duration;i+=20){
        events+=(unsigned)camera_reset_button_sample(&s,level,time_ms,busy);time_ms+=20;
    }
}
static void init(void){camera_reset_button_init(&s);events=0;time_ms=0;}
int main(void)
{
    init();feed(1,400,0);feed(0,6000,0);assert(!events);feed(1,300,0);assert(events==1);
    feed(1,2000,0);assert(events==1);feed(0,6000,0);feed(1,300,0);assert(events==2);
    init();feed(1,400,0);feed(0,3000,0);feed(1,400,0);assert(!events);
    init();feed(0,15000,0);feed(1,400,0);assert(!events);feed(0,6000,0);feed(1,300,0);assert(events==1);
    init();feed(1,400,0);feed(0,3000,0);feed(-1,20,0);feed(0,6000,0);feed(1,400,0);assert(!events);
    init();feed(1,400,0);feed(0,3000,0);feed(0,4000,1);feed(0,6000,0);feed(1,400,0);assert(!events);
    init();feed(1,400,0);feed(0,3000,0);time_ms+=1000;feed(0,4000,0);feed(1,400,0);assert(!events);
    init();feed(1,400,0);feed(0,3000,0);time_ms=100;feed(0,4000,0);feed(1,400,0);assert(!events);
    init();feed(1,400,0);feed(0,3000,0);feed(1,20,0);feed(0,3000,0);feed(1,400,0);assert(events==1);
    init();feed(1,400,0);for(unsigned i=0;i<300;i++){feed(0,20,0);feed(1,20,0);}assert(!events);
    puts("PASS reset button: long press/release, short press, startup-held, faults, OTA busy, gaps, clock regression, bounce and repeated gestures");
}
