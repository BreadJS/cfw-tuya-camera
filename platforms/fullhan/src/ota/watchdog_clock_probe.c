/* Proposed read-only probe for the exact FH8626V100 kernel identified below.
 * No register, RAM, file, MTD or watchdog writes. Not an installation lease.
 * Physical PMU mapping is derived from the shipped kernel's iotable entry.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "watchdog_clock_probe.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>

_Static_assert(sizeof(off_t)==8,"64-bit physical offsets required");
static const unsigned char setter[] = {0x21,0x0c,0x50,0xe3,0x06,0x00,0x00,0x8a,0x00,0x30,0xa0,0xe3,0x9a,0x3f,0x07,0xee,0x14,0x30,0x9f,0xe5,0x00,0x30,0x93,0xe5,0x00,0x00,0x83,0xe0,0x00,0x10,0x80,0xe5,0x1e,0xff,0x2f,0xe1,0x04,0x00,0x9f,0xe5,0xbf,0x92,0x01,0xea,0xd4,0x1d,0x4f,0xc0};
static const unsigned char getter[] = {0x21,0x0c,0x50,0xe3,0x06,0x00,0x00,0x8a,0x28,0x30,0x9f,0xe5,0x00,0x30,0x93,0xe5,0x00,0x00,0x83,0xe0,0x00,0x00,0x90,0xe5,0x00,0x30,0xa0,0xe3,0x9a,0x3f,0x07,0xee,0x1e,0xff,0x2f,0xe1,0x10,0x40,0x2d,0xe9,0x0c,0x00,0x9f,0xe5,0xb1,0x92,0x01,0xeb,0x00,0x00,0xa0,0xe3,0x10,0x80,0xbd,0xe8,0xd4,0x1d,0x4f,0xc0,0x98,0x4b,0x43,0xc0};
static const unsigned char pause_code[] = {0x10,0x40,0x2d,0xe9,0x7f,0x0f,0xa0,0xe3,0x61,0xfe,0xff,0xeb,0x01,0x1c,0x80,0xe3,0x7f,0x0f,0xa0,0xe3,0x51,0xfe,0xff,0xeb,0x04,0x00,0x9f,0xe5,0x10,0x40,0xbd,0xe8,0x19,0x91,0x01,0xea,0xa0,0x50,0x43,0xc0};

static int ram(const char *path)
{
    struct statfs fs;
    return !statfs(path,&fs)&&((unsigned long)fs.f_type==0x01021994UL||(unsigned long)fs.f_type==0x858458f6UL);
}
static int read_fixed(int fd,uint64_t physical,void *out,size_t size)
{
    uint64_t page=physical&~UINT64_C(4095);size_t offset=(size_t)(physical-page);
    if(!size||offset+size>4096)return -1;
    void *mapping=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,(off_t)page);
    if(mapping==MAP_FAILED)return -1;
    const volatile unsigned char *source=(const volatile unsigned char *)mapping+offset;
    unsigned char *destination=out;
    for(size_t i=0;i<size;i++)destination[i]=source[i];
    return munmap(mapping,4096);
}
static int fingerprint(int fd,uint64_t physical,const unsigned char *expected,size_t size)
{
    unsigned char actual[64];
    return size>sizeof actual||read_fixed(fd,physical,actual,size)||memcmp(actual,expected,size)?-1:0;
}
static int failed(char *error,size_t size,const char *why)
{if(size)snprintf(error,size,"%s",why);return -1;}
int camera_watchdog_clock_inspect(struct camera_watchdog_clock_report *report,
                                 char *error,size_t size)
{
    if(!report||(!error&&size))return -1;
    memset(report,0,sizeof *report);if(size)error[0]=0;
    struct utsname u;
    if(geteuid()!=0||sysconf(_SC_PAGESIZE)!=4096||uname(&u)||strcmp(u.machine,"armv6l")||
       strcmp(u.release,"4.9.129")||strcmp(u.version,"#55 PREEMPT Sat Dec 11 11:44:24 CST 2021")||
       !ram("/")||!ram("/proc/1/exe"))return failed(error,size,"unsupported_kernel_or_non_RAM_runtime");
    struct statfs fs;
    if(statfs("/sys",&fs)||(unsigned long)fs.f_type!=0x62656572UL||
       statfs("/proc",&fs)||(unsigned long)fs.f_type!=0x9fa0UL)
        return failed(error,size,"sysfs_or_procfs_unavailable");
    int fd=open("/dev/mem",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return failed(error,size,"devmem_unavailable");
    struct stat st;int valid=!fstat(fd,&st)&&S_ISCHR(st.st_mode)&&st.st_uid==0&&
        major(st.st_rdev)==1&&minor(st.st_rdev)==1;
    uint32_t pmu_state[2]={0};
    if(!valid||fingerprint(fd,UINT64_C(0xa001a160),setter,sizeof setter)||
       fingerprint(fd,UINT64_C(0xa001a194),getter,sizeof getter)||
       fingerprint(fd,UINT64_C(0xa001a800),pause_code,sizeof pause_code)){
        close(fd);return failed(error,size,"kernel_fingerprint_mismatch");
    }
    report->fingerprint_valid=1;
    if(read_fixed(fd,UINT64_C(0xa04f1dd4),pmu_state,sizeof pmu_state)||
       pmu_state[0]!=UINT32_C(0xfe090000)||pmu_state[1]!=1){
        close(fd);return failed(error,size,"PMU_mapping_mismatch");
    }
    report->mapping_valid=1;
    /* Fixed kernel descriptor: VAfe090000, PFNf0000, length4000. Do not
     * convert or trust an arbitrary runtime pointer as an MMIO address. */
    void *page=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,(off_t)UINT64_C(0xf0000000));
    if(page==MAP_FAILED){close(fd);return failed(error,size,"PMU_read_mapping_failed");}
    const volatile uint32_t *word=(const volatile uint32_t *)((const unsigned char *)page+0x1fc);
    uint32_t first=*word,second=*word;
    report->first_value=first;report->second_value=second;report->read_complete=1;
    report->stable=first==second;report->clock_paused=(first&second&256)!=0;
    if(munmap(page,4096))report->cleanup_error=1;
    if(close(fd))report->cleanup_error=1;
    int registered=access("/sys/class/misc/watchdog",F_OK);
    if(registered&&errno!=ENOENT)return failed(error,size,"watchdog_registration_unknown");
    report->driver_registered=registered==0;
    if(report->cleanup_error)return failed(error,size,"read_mapping_cleanup_failed");
    if(!report->stable||!report->clock_paused||report->driver_registered)
        return failed(error,size,"watchdog_not_stably_paused_or_driver_registered");
    return 0;
}
#ifndef CAMERA_WATCHDOG_PROBE_LIBRARY
int main(int argc,char **argv)
{
    if(argc!=2||strcmp(argv[1],"--inspect"))return 2;
    struct camera_watchdog_clock_report report;char error[128];
    int rc=camera_watchdog_clock_inspect(&report,error,sizeof error);
    if(!report.read_complete){fprintf(stderr,"WATCHDOG_CLOCK_REFUSED reason=%s no_writes=yes\n",error);return 2;}
    printf("{\"read_only\":true,\"physical_register\":\"0xf00001fc\","
           "\"pause_mask\":256,\"first_value\":%u,\"second_value\":%u,"
           "\"stable\":%s,\"watchdog_clock_paused\":%s,\"watchdog_registered\":%s,"
           "\"lease_granted\":false}\n",report.first_value,report.second_value,report.stable?"true":"false",
           report.clock_paused?"true":"false",report.driver_registered?"true":"false");
    if(rc)fprintf(stderr,"WATCHDOG_CLOCK_REFUSED reason=%s no_writes=yes\n",error);
    return rc?3:0;
}
#endif
