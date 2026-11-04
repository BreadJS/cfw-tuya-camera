#define _GNU_SOURCE
#include "../native/control/credentials.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
static void wipe(void *data,size_t size){volatile unsigned char *p=data;while(size--)*p++=0;}
static void timed_out(int signal_number){(void)signal_number;_exit(1);}
static int bootstrap(const char *path,char out[256])
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;ssize_t n=read(fd,out,255);
    int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1||n<3||st.st_size!=n;
    close(fd);if(bad)return -1;
    out[n]=0;while(n>0&&(out[n-1]=='\n'||out[n-1]=='\r'))out[--n]=0;
    return strlen(out)==(size_t)n?0:-1;
}
static int load(const char *directory,const char *initial)
{
    char auth[256]={0};if(bootstrap(initial,auth)){wipe(auth,sizeof auth);return -1;}
    int fd=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    int rc=fd<0?-1:fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&077)?-1:camera_credentials_open(fd,auth);
    if(fd>=0)close(fd);
    wipe(auth,sizeof auth);return rc;
}
static int line(char *out,size_t size)
{
    if(!fgets(out,(int)size,stdin))return -1;
    size_t n=strlen(out);
    if(n&&out[n-1]=='\n'){out[--n]=0;if(n&&out[n-1]=='\r')out[--n]=0;return 0;}
    int c;while((c=getchar())!=EOF&&c!='\n'){}
    wipe(out,size);return -1;
}
#ifndef CAMERA_TELNET_LOGIN_TEST
/* Immutable ROOTFS has no /root. Shell setup must use the writable RAM /tmp. */
static int authenticated_shell(void)
{
    puts("Authenticated. Root shell.");umask(077);
    struct rlimit limit;if(getrlimit(RLIMIT_NOFILE,&limit))return 1;
    unsigned count=limit.rlim_cur>65536?65536:(unsigned)limit.rlim_cur;
    for(unsigned fd=3;fd<count;fd++)close((int)fd);
    if(clearenv()||setenv("HOME",CAMERA_LOGIN_HOME,1)||setenv("USER","root",1)||
       setenv("LOGNAME","root",1)||setenv("SHELL","/bin/sh",1)||
       setenv("TERM","vt100",1)||setenv("PATH",CAMERA_LOGIN_PATH,1)||chdir(CAMERA_LOGIN_HOME))return 1;
    execl("/bin/sh","-sh",(char *)NULL);return 1;
}
#endif
int main(int argc,char **argv)
{
    const char *directory=CAMERA_CONFIG_DIRECTORY,*initial=CAMERA_LOGIN_INITIAL;
#ifdef CAMERA_TELNET_LOGIN_TEST
    if(argc!=3)return 2;
    directory=argv[1];initial=argv[2];
#else
    (void)argc;(void)argv;
    if(geteuid()!=0)return 1;
#endif
    if(!isatty(0)||!isatty(1))return 1;
    setvbuf(stdout,NULL,_IONBF,0);signal(SIGALRM,timed_out);
    struct termios original;if(tcgetattr(0,&original))return 1;
    puts("Camera Telnet — use your web login.");
    for(unsigned attempt=0;attempt<3;attempt++){
        char user[66]={0},password[130]={0};alarm(90);
        fputs("Login: ",stdout);if(line(user,sizeof user))return 1;
        struct termios hidden=original;hidden.c_lflag&=~(ECHO|ECHONL);
        if(tcsetattr(0,TCSAFLUSH,&hidden))return 1;
        fputs("Password: ",stdout);int rc=line(password,sizeof password);
        tcsetattr(0,TCSAFLUSH,&original);putchar('\n');alarm(0);
        if(rc){wipe(password,sizeof password);return 1;}
        if(load(directory,initial)){wipe(password,sizeof password);puts("Login unavailable.");return 1;}
        int good=camera_credentials_verify(0,user,password);
        wipe(password,sizeof password);wipe(user,sizeof user);
        if(good){
#ifdef CAMERA_TELNET_LOGIN_TEST
            puts("AUTH_OK");return 0;
#else
            return authenticated_shell();
#endif
        }
        puts("Login incorrect.");sleep(1);
    }
    return 1;
}
