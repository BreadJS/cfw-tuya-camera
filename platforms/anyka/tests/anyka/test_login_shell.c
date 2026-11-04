/* Exercise the actual production post-auth path; no shell is executed. */
#define _GNU_SOURCE
#define main camera_login_main
#define execl mock_execl
#include "../../src/telnet/login.c"
#undef main
#undef execl
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
static int inherited;
int mock_execl(const char *path,const char *arg,...)
{
    assert(!strcmp(path,"/bin/sh")&&!strcmp(arg,"-sh"));
    va_list args;va_start(args,arg);assert(va_arg(args,char*)==NULL);va_end(args);
    char cwd[256];assert(getcwd(cwd,sizeof cwd)&&!strcmp(cwd,"/tmp"));
    assert(getenv("HOME")&&!strcmp(getenv("HOME"),"/tmp"));
    assert(!strcmp(getenv("USER"),"root")&&!strcmp(getenv("LOGNAME"),"root"));
    assert(!strcmp(getenv("SHELL"),"/bin/sh")&&!strcmp(getenv("TERM"),"vt100"));
    assert(!strcmp(getenv("PATH"),"/tmp/camera-runtime:/bin:/sbin:/usr/bin:/usr/sbin"));
    assert(!getenv("LD_PRELOAD")&&!getenv("LD_LIBRARY_PATH")&&!getenv("UNTRUSTED_LOGIN_ENV"));
    errno=0;assert(fcntl(inherited,F_GETFD)<0&&errno==EBADF);
    mode_t old=umask(077);assert(old==077);
    _exit(0);
}
int main(void)
{
    pid_t child=fork();assert(child>=0);
    if(!child){
        assert(!setenv("LD_PRELOAD","untrusted.so",1));assert(!setenv("LD_LIBRARY_PATH","/untrusted",1));assert(!setenv("UNTRUSTED_LOGIN_ENV","secret",1));
        inherited=open("/dev/null",O_RDONLY);assert(inherited>=3);
        if(chdir("/"))_exit(10);
        (void)authenticated_shell();_exit(11);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    puts("PASS production Telnet shell: /tmp HOME/cwd, private umask, clean environment, inherited descriptors closed and login-shell exec");
}
