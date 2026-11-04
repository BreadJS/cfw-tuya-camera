/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sensor_timing.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Exact anyka_ipc: get/set_fps_level @d2634/d245c transfer nine32-bit
 * fields. Each triple is FPS, maximum exposure rows, gain threshold.
 * Ak_ISP_Set_Sensor_Fps @da2f8 uses a4-byte pointer (ioctl4004497a).
 * Ak_ISP_Get_Sensor_Fps @da308 returns its local ioctl8004497b value.
 * VI attributes alone copy/log FPS; they do not program sensor timing. */
int anyka_sensor_timing_bind(struct anyka_sensor_timing *out)
{
    if (!out) return -EINVAL;
    struct anyka_sensor_timing t={0}; void *symbol;
#define BIND(member,name) do { symbol=dlsym(RTLD_DEFAULT,name); if(!symbol){memset(out,0,sizeof *out);return -ENOSYS;} _Static_assert(sizeof t.member==sizeof symbol,"POSIX pointer");memcpy(&t.member,&symbol,sizeof symbol); } while(0)
    BIND(sensor_get,"Ak_ISP_Get_Sensor_Fps");
    BIND(sensor_set,"Ak_ISP_Set_Sensor_Fps");
    BIND(levels_get,"ak_vpss_get_fps_level");
    BIND(levels_set,"ak_vpss_set_fps_level");
#undef BIND
    *out=t;return 0;
}
static int levels_read(struct anyka_sensor_timing *t,uint32_t levels[9])
{
    long page=sysconf(_SC_PAGESIZE);
    if(page<4096||page>65536)return -EINVAL;
    unsigned char *base=mmap(NULL,(size_t)page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(base==MAP_FAILED)return -errno;
    if(mprotect(base+page,(size_t)page,PROT_READ|PROT_WRITE)){int rc=-errno;munmap(base,(size_t)page*3);return rc;}
    unsigned char *data=base+2*page-36;memset(data-16,0xa5,52);
    int rc=t->levels_get(0,data);
    for(unsigned i=0;i<16;i++)if(data[-16+(int)i]!=0xa5)rc=-EOVERFLOW;
    if(!rc)memcpy(levels,data,36);
    munmap(base,(size_t)page*3);return rc?rc<0?rc:-EIO:0;
}
static int timing_failure(const char *stage,int rc)
{fprintf(stderr,"ANYKA_SENSOR_TIMING_FAILURE stage=%s rc=%d\n",stage,rc);fflush(stderr);return rc;}
static void timing_levels(const char *stage,const uint32_t levels[9])
{printf("ANYKA_SENSOR_TIMING_LEVELS stage=%s high=%u/%u/%u mid=%u/%u/%u low=%u/%u/%u\n",stage,levels[0],levels[1],levels[2],levels[3],levels[4],levels[5],levels[6],levels[7],levels[8]);fflush(stdout);}
int anyka_sensor_timing_apply(struct anyka_sensor_timing *t,unsigned fps)
{
    if(!t||!t->sensor_get||!t->sensor_set||!t->levels_get||!t->levels_set||(fps!=15&&fps!=25))return -EINVAL;
    uint32_t original[9],desired[9],actual[9];
    int rc=levels_read(t,original);if(rc)return timing_failure("levels-initial-read",rc);
    timing_levels("initial",original);
    memcpy(desired,original,sizeof desired);
    unsigned maximum=28125u/fps-1u;
    /* The exact setter treats the middle level as inactive if any of its
     * three fields is zero. Preserve this policy and all gain thresholds. */
    for(unsigned i=0;i<9;i+=3){
        if(i==3&&(!original[3]||!original[4]||!original[5]))continue;
        if(!original[i]||original[i]>120||!original[i+1]||original[i+1]>65535)return timing_failure("levels-initial-range",-EPROTO);
        desired[i]=fps;
        if(desired[i+1]>maximum)desired[i+1]=maximum;
    }
    if(memcmp(original,desired,sizeof desired)){
        timing_levels("request",desired);
        rc=t->levels_set(0,desired);if(rc)return timing_failure("levels-set",rc<0?rc:-EIO);
        rc=levels_read(t,actual);if(rc)return timing_failure("levels-readback",rc);
        timing_levels("readback",actual);
        if(memcmp(desired,actual,sizeof desired))return timing_failure("levels-readback-match",-EIO);
    }
    int before=t->sensor_get(0),requested=(int)fps;
    printf("ANYKA_SENSOR_TIMING_SENSOR stage=before fps=%d requested=%u\n",before,fps);fflush(stdout);
    if(before<=0||before>120)return timing_failure("sensor-initial-range",-EPROTO);
    /* Push even an equal request: the kernel and userspace profile can have
     * inherited different timing state from the prior stopped application. */
    rc=t->sensor_set(0,&requested);if(rc)return timing_failure("sensor-set",rc<0?rc:-EIO);
    struct timespec start,now;
    if(clock_gettime(CLOCK_MONOTONIC,&start))return timing_failure("sensor-readback-clock",-errno);
    int after;unsigned waited_ms=0;
    do{
        after=t->sensor_get(0);
        if(clock_gettime(CLOCK_MONOTONIC,&now))return timing_failure("sensor-readback-clock",-errno);
        int64_t elapsed=(int64_t)(now.tv_sec-start.tv_sec)*INT64_C(1000000000)+(int64_t)now.tv_nsec-start.tv_nsec;
        waited_ms=elapsed>0?(unsigned)(elapsed/1000000):0;
        if(after==(int)fps||waited_ms>=500||after<=0||after>120)break;
        /* The sensor setter can acknowledge a pending frame-boundary change.
         * Wait within the current owner; never reopen capture to force it. */
        usleep(10000);
    }while(1);
    printf("ANYKA_SENSOR_TIMING_SENSOR stage=readback fps=%d requested=%u waited_ms=%u\n",after,fps,waited_ms);fflush(stdout);
    if(after!=(int)fps)return timing_failure("sensor-readback-match",-EIO);
    printf("ANYKA_SENSOR_TIMING requested=%u before=%d readback=%d max_rows=%u levels=%u/%u/%u measured_cadence=pending\n",fps,before,after,maximum,desired[0],desired[3],desired[6]);
    fflush(stdout);return 0;
}
