#include "../../src/ota/boot_runtime.c"
#include <assert.h>
#include <stdlib.h>
static unsigned watchdog_calls;
int camera_watchdog_clock_inspect(struct camera_watchdog_clock_report *report,char *error,size_t size)
{(void)report;(void)error;(void)size;watchdog_calls++;abort();}
static void parsers(void)
{
    static const char *const valid[]={
        "root=/dev/mtdblock4",
        "console=ttyS0,115200 mem=36M quiet root=/dev/mtdblock4 rootfstype=squashfs mtdparts=spi_flash:256k(bootstrap),64k(uboot-env),256k(uboot),1856k(kernel),5440k(rootfs),320k(config)\n",
        "\troot=/dev/mtdblock4\trootfstype=squashfs ro\n"
    };
    for(unsigned i=0;i<sizeof valid/sizeof valid[0];i++)assert(camera_boot_cmdline_is_installed(valid[i],strlen(valid[i])));
    static const char *const invalid[]={
        "", "quiet", "root=/dev/mmcblk0p1", "root=/dev/mtdblock40", "xroot=/dev/mtdblock4",
        "root=/dev/mtdblock4 root=/dev/mtdblock4", "root=/dev/mtdblock4 root=/dev/mmcblk0p1",
        "root=/dev/mtdblock4 init=/sbin/init", "root=/dev/mtdblock4 init", "root=/dev/mtdblock4 rdinit=/RAMINIT",
        "root=/dev/mtdblock4 camera_sd_test=0", "root=/dev/mtdblock4 camera_recovery=0",
        "root=/dev/mtdblock4 recovery", "root=/dev/mtdblock4 recovery_mode=1", "root=/dev/mtdblock4 rescue=1",
        "root=/dev/mtdblock4 single", "root=/dev/mtdblock4 S", "root=/dev/mtdblock4 -- test",
        "root=/dev/mtdblock4 -s", "root=/dev/mtdblock4 1", "root=/dev/mtdblock4 emergency",
        "root=/dev/mtdblock4 rd.break", "root=/dev/mtdblock4 systemd.unit=rescue.target",
        "root=/dev/mtdblock4 rw", "root=/dev/mtdblock4 rootfstype=vfat",
        "root=/dev/mtdblock4 rootfstype=squashfs rootfstype=squashfs", "root=\"/dev/mtdblock4\""
    };
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++)assert(!camera_boot_cmdline_is_installed(invalid[i],strlen(invalid[i])));
    const char embedded[]="root=/dev/mtdblock4\0recovery=1";assert(!camera_boot_cmdline_is_installed(embedded,sizeof embedded-1));
    char large[4097];memset(large,' ',sizeof large);assert(!camera_boot_cmdline_is_installed(large,sizeof large));
    assert(!camera_boot_cmdline_is_installed(NULL,1));
    uint8_t id[16],before[16];memset(before,0x55,sizeof before);memcpy(id,before,sizeof id);
    const char *uuid="00112233-4455-4677-8899-aabbccddeeff";
    assert(!camera_boot_parse_id(uuid,strlen(uuid),id));assert(id[0]==0&&id[1]==0x11&&id[15]==0xff);
    assert(!camera_boot_parse_id("00112233-4455-4677-8899-AABBCCDDEEFF\n",37,id));
    static const char *const bad_uuid[]={"", "00112233445546778899aabbccddeeff", "00112233_4455-4677-8899-aabbccddeeff", "00112233-4455-4677-8899-aabbccddeefg", "00000000-0000-0000-0000-000000000000", "00112233-4455-4677-8899-aabbccddeeffx", "00112233-4455-4677-8899-aabbccddeeff\r\n"};
    for(unsigned i=0;i<sizeof bad_uuid/sizeof bad_uuid[0];i++){memcpy(id,before,sizeof id);assert(camera_boot_parse_id(bad_uuid[i],strlen(bad_uuid[i]),id));assert(!memcmp(id,before,sizeof id));}
    puts("PASS strict boot-argument and boot-ID parsers");
}
static void devices(void)
{
    struct stat st={.st_mode=S_IFCHR|0600,.st_uid=0,.st_rdev=makedev(90,6)};
    assert(character(&st,3));assert(!character(&st,4));st.st_rdev=makedev(90,7);assert(!character(&st,3));
    st.st_rdev=makedev(31,3);assert(!character(&st,3));st.st_rdev=makedev(90,6);st.st_uid=1000;assert(!character(&st,3));
    st.st_uid=0;st.st_mode=S_IFCHR|0666;assert(!character(&st,3));st.st_mode=S_IFREG|0600;assert(!character(&st,3));
    struct mtd_info_user info={.type=MTD_NORFLASH,.flags=MTD_WRITEABLE|MTD_BIT_WRITEABLE,.size=OTA_KERNEL_LIMIT,.erasesize=OTA_ERASE_SIZE,.writesize=1};
    assert(info_ok(3,&info));assert(!info_ok(4,&info));assert(!info_ok(2,&info));
    info.flags=MTD_BIT_WRITEABLE;assert(info_ok(3,&info));info.flags|=MTD_NO_ERASE;assert(!info_ok(3,&info));
    info.flags=MTD_BIT_WRITEABLE;info.size++;assert(!info_ok(3,&info));info.size=OTA_KERNEL_LIMIT;info.erasesize*=2;assert(!info_ok(3,&info));
    info.erasesize=OTA_ERASE_SIZE;info.writesize=2;assert(!info_ok(3,&info));info.writesize=1;info.oobsize=1;assert(!info_ok(3,&info));
    info.oobsize=0;info.type=MTD_NANDFLASH;assert(!info_ok(3,&info));
    puts("PASS fixed read-only NOR identity and geometry predicates");
}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"parsers")){parsers();devices();assert(!watchdog_calls);return 0;}
    if(argc==3&&!strcmp(argv[1],"needed")){printf("%d\n",camera_boot_confirmation_needed(argv[2]));assert(!watchdog_calls);return 0;}
    if(argc==3&&!strcmp(argv[1],"unsupported")){
        struct ota_boot_result result;char error[256];
        int rc=camera_boot_confirm_installed("/does-not-exist",argv[2],1,&result,error,sizeof error);
        assert(rc<0&&!watchdog_calls&&!result.accepted_committed);printf("PASS unsupported host refused before watchdog/MTD access: %s\n",error);return 0;
    }
    return 2;
}
