/* Source/fake integration only: old fake syscall table never opens devices. */
#define main retained_backend_fixture_main
#include "../../../ota/tests/linux_mtd_driver.c"
#undef main
#include "../../src/ota/ram_install.h"
#include "../../src/ota/boot_confirm.h"
#include <fcntl.h>
#include <unistd.h>
static int test_progress_fd=-1;

static int environment_test(void *context,const struct ota_apply_plan *plan,enum ota_mtd_phase phase)
{
    struct fake *f=context;
    if(is(f,"progress_lost_at_erase")&&phase==OTA_MTD_ERASE){if(fchmod(test_progress_fd,0600))return -1;}
    if(is(f,"wrong_origin")||is(f,"volatile_config")||is(f,"watchdog_unpaused")||is(f,"old_NOR_mapping"))return -1;
    return fake_lease(context,plan,phase,OTA_MTD_SYNC_RELEASE49);
}
static int prepare_test(void *context,struct ota_apply_plan *plan,const uint8_t id[16],char *error,size_t size)
{
    struct fake *f=context;
    if(is(f,"journal_failure"))return -1;
    return ota_boot_prepare(plan,id,NULL,NULL,error,size);
}
int main(int argc,char **argv)
{
    if(argc==5&&!strcmp(argv[1],"--exit")){
        struct ram_install_result value={0};value.may_have_changed=(unsigned)atoi(argv[3]);value.verified_written=(unsigned)atoi(argv[4]);
        printf("{\"status\":%d}\n",ram_install_exit_status(atoi(argv[2]),&value));return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--cmdline")){
        printf("{\"accepted\":%s}\n",ram_install_test_cmdline(argv[2])?"false":"true");return 0;
    }
    if(argc!=7)return 2;
    struct ota_apply_plan plan;char error[256]="";
    if(ota_apply_plan_open(&plan,argv[1],argv[2],argv[3],argv[4],error,sizeof error))return 2;
    struct fake f={.plan=&plan,.scenario=argv[5]};f.flash=malloc(OTA_FLASH_SIZE);if(!f.flash)return 2;
    memset(f.flash,0xa5,OTA_FLASH_SIZE);
    const struct ota_mtd_syscalls sys={fake_open,fake_close,fake_fstat,fake_lstat,fake_read,fake_pread,fake_pwrite,fake_ioctl,fake_sync,fake_map};
    struct ram_install_test_ops ops={&f,environment_test,prepare_test,&sys,&f};
    int report=open(argv[6],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(report<0)return 2;
    test_progress_fd=report;
    const uint8_t id[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    struct ram_install_result result;
    int rc=ram_install_test_execute(&plan,report,id,&ops,&result,error,sizeof error);
    if(is(&f,"progress_lost_at_erase")&&fchmod(report,0700))return 2;
    unsigned leaked=0,protected_changed=0;
    for(unsigned i=0;i<32;i++)leaked+=f.fd[i].used!=0;
    for(unsigned p=0;p<OTA_MTD_COUNT;p++)if(p!=3&&p!=4)
        for(uint32_t j=0;j<plan.map.part[p].size;j++)protected_changed+=f.flash[plan.map.part[p].flash_offset+j]!=0xa5;
    printf("{\"rc\":%d,\"journal_prepared\":%u,\"may_have_changed\":%u,\"verified_written\":%u,"
        "\"completed\":%u,\"erases\":%u,\"writes\":%u,\"reads\":%u,\"device_opens\":%u,"
        "\"leaked\":%u,\"protected_changed\":%u,\"forbidden\":%u,\"bad_io\":%u}\n",rc,result.journal_prepared,
        result.may_have_changed,result.verified_written,result.progress.blocks_completed,f.erases,f.writes,f.reads,f.device_opens,
        leaked,protected_changed,f.forbidden,f.bad_io);
    if(rc)fprintf(stderr,"%s\n",error);
    close(report);free(f.flash);ota_apply_plan_close(&plan);return 0;
}
