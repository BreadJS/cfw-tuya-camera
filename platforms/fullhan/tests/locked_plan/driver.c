#define _POSIX_C_SOURCE 200809L
#include "../../src/ota/ota_apply_plan.h"
#include "../../src/ota/ota_state.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    assert(argc==6);uint8_t key[32];char error[256];
    assert(!ota_read_public_key(argv[1],key,1,error,sizeof error));
    struct ota_state state;assert(!ota_state_open(&state,argv[2],argv[3],key,error,sizeof error));
    struct ota_apply_plan ordinary;
    assert(ota_apply_plan_open(&ordinary,argv[1],argv[2],argv[3],argv[4],error,sizeof error));
    if(!strcmp(argv[5],"corrupt-capsule")){
        int fd=openat(state.staging,"pending.cap",O_RDWR);assert(fd>=0);
        off_t end=lseek(fd,-1,SEEK_END);assert(end>0);unsigned char byte;
        assert(pread(fd,&byte,1,end)==1);byte^=1;assert(pwrite(fd,&byte,1,end)==1);close(fd);
    }
    int lock=state.lock,stage=state.staging,extra=-1;
    if(!strcmp(argv[5],"wrong-lock")){
        extra=openat(state.directory,"unrelated-lock",O_CREAT|O_EXCL|O_RDWR,0600);assert(extra>=0);lock=extra;
    }
    if(!strcmp(argv[5],"same-directories"))stage=state.directory;
    struct ota_apply_plan retained;int rc=ota_apply_plan_open_locked(&retained,state.directory,stage,lock,key,argv[4],error,sizeof error);
    if(strcmp(argv[5],"valid"))assert(rc);
    else {
        assert(!rc&&retained.verified.sequence==2&&retained.block_count==114);
        for(unsigned i=0;i<retained.block_count;i++)assert(retained.blocks[i].mtd_index==3||retained.blocks[i].mtd_index==4);
        ota_state_close(&state);assert(!ota_apply_plan_guard(&retained,error,sizeof error));
        assert(ota_state_open(&state,argv[2],argv[3],key,error,sizeof error));
        ota_apply_plan_close(&retained);
        assert(!ota_state_open(&state,argv[2],argv[3],key,error,sizeof error));
    }
    if(extra>=0)close(extra);
    ota_state_close(&state);puts("PASS retained authenticated plan and exclusive lock ownership");return 0;
}
