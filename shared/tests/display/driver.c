#define _GNU_SOURCE
#include "../../src/display/settings.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    if(argc<2)return 2;
    if(!strcmp(argv[1],"clock")&&argc==4){char b[24];setenv("TZ","Pacific/Honolulu",1);tzset();int rc=camera_display_format_clock((time_t)strtoll(argv[2],NULL,10),(unsigned)strtoul(argv[3],NULL,10),b);if(!rc)puts(b);return rc?1:0;}
    if(argc<3)return 2;
    int dir=open(argv[2],O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(dir<0)return 2;
    struct camera_display_settings s;camera_display_defaults(&s);int rc=-1;
    if(!strcmp(argv[1],"load")){
        struct camera_display_settings before=s;rc=camera_display_load(dir,&s);
        if(rc&&memcmp(&s,&before,sizeof s))return 3;
        if(!rc)printf("%u %u %s\n",s.timestamp_enabled,s.timezone,s.name);
    }else if((!strcmp(argv[1],"save")||!strcmp(argv[1],"valid"))&&argc==6){
        memset(s.name,0,sizeof s.name);size_t n=strlen(argv[3]);if(n>128||n%2)return 2;
        for(size_t i=0;i<n/2;i++){char byte[3]={argv[3][i*2],argv[3][i*2+1],0};s.name[i]=(char)strtoul(byte,NULL,16);}
        s.timestamp_enabled=(unsigned)strtoul(argv[4],NULL,10);s.timezone=(unsigned)strtoul(argv[5],NULL,10);
        rc=!strcmp(argv[1],"valid")?!camera_display_valid(&s):camera_display_save(dir,&s);
    }
    close(dir);return rc?1:0;
}
