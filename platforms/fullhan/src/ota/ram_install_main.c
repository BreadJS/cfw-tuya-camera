#define _POSIX_C_SOURCE 200809L
#include "ram_install.h"
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    if(argc!=2||strcmp(argv[1],"--apply-inherited")){
        fputs("Usage: camera-ram-install --apply-inherited (trusted exec descriptors3..7 only)\n",stderr);return 2;
    }
    const struct ram_install_fds fds={3,4,5,6,7};struct ram_install_result result;char error[256];
    int rc=ram_install_apply_inherited(&fds,&result,error,sizeof error);
    if(rc)fprintf(stderr,"RAM_INSTALL_FAILED reason=%s journal_prepared=%u may_have_changed=%u verified_written=%u no_reboot=yes\n",
        error,result.journal_prepared,result.may_have_changed,result.verified_written);
    else puts("RAM_INSTALL_WRITTEN_VERIFIED reboot_pending=yes accepted_advanced=no");
    return ram_install_exit_status(rc,&result);
}
