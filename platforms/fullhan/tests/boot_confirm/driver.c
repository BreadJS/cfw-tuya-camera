#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "../../src/ota/boot_confirm.h"
#include "../../src/ota/ota_state.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
struct context {int file[2],directory,mutated;const char *fault;};
static int fault(void *opaque,const char *point)
{struct context *c=opaque;return !strcmp(c->fault,point);}
static ssize_t installed_read(void *opaque,unsigned mtd,uint32_t offset,void *data,size_t size)
{
    struct context *c=opaque;
    if(mtd<3||mtd>4){errno=EINVAL;return -1;}
    if(!strcmp(c->fault,"mutate_accepted")&&!c->mutated){
        int fd=openat(c->directory,"accepted",O_WRONLY|O_TRUNC|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0||write(fd,"9\n",2)!=2||fsync(fd))abort();
        close(fd);c->mutated=1;
    }
    if(!strcmp(c->fault,"short_reads")&&offset<256&&size>7)size=7;
    return pread(c->file[mtd-3],data,size,offset);
}
static int boot_id(const char *text,uint8_t id[16])
{
    if(strlen(text)!=32)return -1;
    for(unsigned i=0;i<16;i++){unsigned v;char b[3]={text[i*2],text[i*2+1],0};if(sscanf(b,"%2x",&v)!=1)return -1;id[i]=(uint8_t)v;}
    return 0;
}
int main(int argc,char **argv)
{
    if(argc<2)return 2;
    uint8_t key[32],id[16];char error[256]={0};
    if(!strcmp(argv[1],"state")&&argc==5){
        if(ota_read_public_key(argv[2],key,1,error,sizeof error))return 2;
        struct ota_state state;int rc=ota_state_open(&state,argv[3],argv[4],key,error,sizeof error);
        if(!rc){printf("accepted=%llu pending=%d\n",(unsigned long long)state.accepted,state.has_pending);ota_state_close(&state);}
        else printf("ERROR %s\n",error);
        return rc?1:0;
    }
    if(!strcmp(argv[1],"prepare")&&argc==8){
        if(boot_id(argv[6],id))return 2;
        struct ota_apply_plan plan;
        if(ota_apply_plan_open(&plan,argv[2],argv[3],argv[4],argv[5],error,sizeof error)){printf("ERROR %s\n",error);return 1;}
        struct context context={.fault=argv[7]};
        int rc=ota_boot_prepare(&plan,id,fault,&context,error,sizeof error);
        ota_apply_plan_close(&plan);printf("rc=%d error=%s\n",rc,error);return rc?1:0;
    }
    if(!strcmp(argv[1],"confirm")&&argc==9){
        if(ota_read_public_key(argv[2],key,1,error,sizeof error)||boot_id(argv[6],id))return 2;
        int dir=open(argv[3],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        int lock=dir<0?-1:openat(dir,"lock",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(dir<0||lock<0)return 2;
        struct context c={.file={open(argv[4],O_RDONLY),open(argv[5],O_RDONLY)},.directory=dir,.fault=argv[8]};
        struct ota_boot_reader reader={.context=&c,.read=installed_read};struct ota_boot_result result;
        int rc=ota_boot_confirm(dir,lock,key,id,strcmp(argv[8],"sd_boot")!=0,atoi(argv[7]),&reader,fault,&c,&result,error,sizeof error);
        printf("rc=%d sequence=%llu already=%u committed=%u cleanup=%u error=%s\n",rc,(unsigned long long)result.sequence,result.already_accepted,result.accepted_committed,result.cleanup_complete,error);
        close(c.file[0]);close(c.file[1]);close(lock);close(dir);
        return rc<0?1:0;
    }
    return 2;
}
