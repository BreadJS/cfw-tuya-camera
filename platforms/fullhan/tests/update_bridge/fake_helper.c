#define _GNU_SOURCE
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#define CHECK(x) do{if(!(x))return 90;}while(0)
int main(int argc,char **argv)
{
    CHECK(argc==2&&!strcmp(argv[1],"--apply-inherited"));
    struct stat s[8],named;for(int i=0;i<8;i++)CHECK(!fstat(i,&s[i]));
    CHECK(S_ISDIR(s[3].st_mode)&&S_ISDIR(s[4].st_mode)&&S_ISDIR(s[7].st_mode));
    CHECK(S_ISCHR(s[0].st_mode));
    CHECK(S_ISREG(s[5].st_mode)&&S_ISREG(s[6].st_mode)&&s[6].st_size==32);
    CHECK(!fstatat(3,"lock",&named,0)&&named.st_ino==s[5].st_ino&&named.st_dev==s[5].st_dev);
    CHECK(!flock(5,LOCK_EX|LOCK_NB));
    for(int i=3;i<=7;i++)CHECK(!(fcntl(i,F_GETFD)&FD_CLOEXEC));
    for(int i=8;i<256;i++)CHECK(fcntl(i,F_GETFD)==-1&&errno==EBADF);
    CHECK((fcntl(0,F_GETFL)&O_ACCMODE)==O_RDONLY&&(fcntl(6,F_GETFL)&O_ACCMODE)==O_RDONLY);
    CHECK(s[1].st_ino==s[2].st_ino&&S_ISREG(s[1].st_mode));CHECK(!getenv("CAMERA_TEST_SECRET"));
    int other=openat(3,"lock",O_RDWR);CHECK(other>=0);CHECK(flock(other,LOCK_EX|LOCK_NB)<0&&(errno==EWOULDBLOCK||errno==EAGAIN));close(other);
    unsigned char expected[32]={0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a},key[32];
    CHECK(pread(6,key,32,0)==32&&!memcmp(key,expected,32));
    int scenario=openat(7,"scenario",O_RDONLY);char text[16]={0};CHECK(scenario>=0&&read(scenario,text,15)>0);close(scenario);
    int cap=openat(4,"pending.cap",O_RDONLY);unsigned char journal[240]={0};CHECK(cap>=0&&pread(cap,journal+32,192,0)==192);close(cap);
    if(!strcmp(text,"tamper"))journal[32+64]^=1;
    memcpy(journal,"FHCBOOT1",8);journal[8]=1;journal[12]=2;
    int j=openat(3,"install-pending",O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(j>=0&&write(j,journal,240)==240);close(j);
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);int receipt=openat(7,"receipt",O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(receipt>=0);
    dprintf(receipt,"%lld\n",(long long)t.tv_sec*1000000000+t.tv_nsec);close(receipt);
    return !strcmp(text,"tamper")?10:atoi(text);
}
