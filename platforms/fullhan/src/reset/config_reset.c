#define _GNU_SOURCE
#include "config_reset.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
static const char marker[]="CAMERA_CONFIG_RESET_V1\n";
static const char *const files[]={"wpa_supplicant.conf","credentials.conf","settings.conf","recovery.conf","illumination.conf","indicator.conf","display.conf","rotation.conf","picture.conf"};
static int fail(char *e,size_t n,const char *why){if(n)snprintf(e,n,"%s",why);return -1;}
static int point(camera_reset_fault f,void *c,const char *p,char *e,size_t n){return f&&f(c,p)?fail(e,n,p):0;}
static int directory(int fd){struct stat s;return fstat(fd,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)?-1:0;}
static int regular(int fd){struct stat s;return fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)||s.st_nlink!=1?-1:0;}
static int marker_present(int dir)
{
    int fd=openat(dir,CAMERA_CONFIG_RESET_MARKER,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT?0:-1;
    char b[sizeof marker];struct stat s;
    int valid=!regular(fd)&&!fstat(fd,&s)&&s.st_size==(off_t)(sizeof marker-1)&&
        pread(fd,b,sizeof b,0)==sizeof marker-1&&!memcmp(b,marker,sizeof marker-1);
    if(close(fd))valid=0;
    return valid?1:-1;
}
/* Verify named inode while retaining the caller's same-description lock. */
static int lock_state(int config,int borrowed,int *local)
{
    *local=-1;
    if(mkdirat(config,"update-state",0700)&&errno!=EEXIST)return -1;
    int state=openat(config,"update-state",O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(state<0)return -1;
    int lock=borrowed;struct stat a,b;
    if(directory(state))goto bad;
    if(lock<0){lock=openat(state,"lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lock<0)goto bad;*local=lock;}
    if(regular(lock)||fstat(lock,&a)||fstatat(state,"lock",&b,AT_SYMLINK_NOFOLLOW)||
       a.st_dev!=b.st_dev||a.st_ino!=b.st_ino||!S_ISREG(b.st_mode)||
       flock(lock,LOCK_EX|LOCK_NB))goto bad;
    if(!fstatat(state,"install-pending",&b,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT)goto bad;
    if(fsync(state)||fsync(config))goto bad;
    close(state);return 0;
bad:
    close(state);if(*local>=0){close(*local);*local=-1;}return -1;
}
static int remove_name(int dir,const char *name)
{
    struct stat s;if(fstatat(dir,name,&s,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?0:-1;
    /* Unlink a name, never traverse a link or recursively remove directories. */
    if(!S_ISREG(s.st_mode)&&!S_ISLNK(s.st_mode))return -1;
    return unlinkat(dir,name,0);
}
static int number(const char **p)
{if(**p<'0'||**p>'9')return 0;while(**p>='0'&&**p<='9')(*p)++;return 1;}
static int temporary(const char *name)
{
    const char *p;
    if(!strcmp(name,".reset-request.new"))return 1;
    const char *one[]={"credentials.tmp.","recovery.conf.tmp."};
    for(unsigned i=0;i<sizeof one/sizeof *one;i++)if(!strncmp(name,one[i],strlen(one[i]))){p=name+strlen(one[i]);return number(&p)&&!*p;}
    const char *two[]={"settings.tmp.","illumination.tmp.","indicator.tmp.","display.tmp.","rotation.tmp.","picture.tmp."};
    for(unsigned i=0;i<sizeof two/sizeof *two;i++)if(!strncmp(name,two[i],strlen(two[i]))){p=name+strlen(two[i]);return number(&p)&&*p++=='.'&&number(&p)&&!*p;}
    if(!strncmp(name,".wifi-",6)){p=name+6;return number(&p)&&*p++=='-'&&number(&p)&&!strcmp(p,".tmp");}
    return 0;
}
static int clear_temporaries(int dir)
{
    /* Independent open description, so caller's directory offset is untouched. */
    int copy=openat(dir,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(copy<0)return -1;
    DIR *d=fdopendir(copy);if(!d){close(copy);return -1;}
    int rc=0;struct dirent *entry;
    for(;;){errno=0;entry=readdir(d);if(!entry){if(errno)rc=-1;break;}
        if(temporary(entry->d_name)&&remove_name(dir,entry->d_name)){rc=-1;break;}}
    if(closedir(d))rc=-1;
    return rc;
}
int camera_config_reset_request(int config,int borrowed,int busy,camera_reset_fault f,void *ctx,char *e,size_t n)
{
    if((n&&!e)||borrowed < -1)return -1;
    if(n)e[0]=0;
    if(busy||directory(config))return fail(e,n,"reset refused: installer busy or unsafe config directory");
    int local;if(lock_state(config,borrowed,&local))return fail(e,n,"reset refused: OTA lock or install journal");
    int rc=-1,present=marker_present(config),fd=-1;
    if(present<0){fail(e,n,"unsafe reset marker");goto done;}
    if(present){rc=fsync(config)?fail(e,n,"reset marker directory sync failed"):0;goto done;}
    if(remove_name(config,".reset-request.new")){fail(e,n,"unsafe reset marker temporary");goto done;}
    fd=openat(config,".reset-request.new",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){fail(e,n,"cannot create reset marker");goto done;}
    const char *p=marker;size_t left=sizeof marker-1;
    while(left){ssize_t wrote=write(fd,p,left);if(wrote<0&&errno==EINTR)continue;if(wrote<=0){fail(e,n,"reset marker write failed");goto done;}p+=wrote;left-=(size_t)wrote;}
    if(fsync(fd)||point(f,ctx,"marker_file_synced",e,n))goto done;
    if(close(fd)){fd=-1;fail(e,n,"reset marker close failed");goto done;}fd=-1;
    if(renameat(config,".reset-request.new",config,CAMERA_CONFIG_RESET_MARKER)||point(f,ctx,"marker_renamed",e,n))goto done;
    if(fsync(config)||point(f,ctx,"marker_directory_synced",e,n))goto done;
    rc=0;
done:
    if(rc&&n&&!e[0])fail(e,n,"reset marker durability failed");
    if(fd>=0)close(fd);
    if(local>=0)close(local);
    return rc;
}
int camera_config_reset_apply(int config,int borrowed,camera_reset_fault f,void *ctx,char *e,size_t n)
{
    if((n&&!e)||borrowed < -1)return -1;
    if(n)e[0]=0;
    if(directory(config))return fail(e,n,"unsafe config directory");
    int present=marker_present(config);if(!present)return 0;if(present<0)return fail(e,n,"unsafe reset marker");
    int local;if(lock_state(config,borrowed,&local))return fail(e,n,"reset refused: OTA lock or install journal");
    int rc=-1;
    if(marker_present(config)!=1){fail(e,n,"reset marker changed");goto done;}
    for(unsigned i=0;i<sizeof files/sizeof *files;i++){
        if(remove_name(config,files[i])){fail(e,n,"cannot unlink own reset file");goto done;}
        if(point(f,ctx,files[i],e,n))goto done;
    }
    if(clear_temporaries(config)||point(f,ctx,"temporaries_removed",e,n))goto done;
    if(fsync(config)||point(f,ctx,"config_directory_synced",e,n))goto done;
    if(unlinkat(config,CAMERA_CONFIG_RESET_MARKER,0)||point(f,ctx,"marker_unlinked",e,n))goto done;
    if(fsync(config)||point(f,ctx,"completed_directory_synced",e,n))goto done;
    rc=1;
done:
    if(rc<0&&n&&!e[0])fail(e,n,"reset removal durability failed");
    if(local>=0)close(local);
    return rc;
}
#ifdef CAMERA_CONFIG_RESET_MAIN
#include <sys/statfs.h>
#include <sys/sysmacros.h>
static int production(void)
{
    struct stat st;struct statfs fs;
    if(geteuid()!=0||statfs("/",&fs)||((unsigned long)fs.f_type!=0x01021994UL&&(unsigned long)fs.f_type!=0x858458f6UL)||
       stat("/etc/conf",&st)||major(st.st_dev)!=31||minor(st.st_dev)!=5||
       statfs("/etc/conf",&fs)||(unsigned long)fs.f_type!=0x72b6UL)return -1;
    int fd=open("/.camera-boot-origin",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    static const char origin[]="NOR_ROOT_V1\n";char b[sizeof origin];
    int good=!regular(fd)&&!fstat(fd,&st)&&(st.st_mode&07777)==0400&&st.st_size==sizeof origin-1&&
        !fstatfs(fd,&fs)&&((unsigned long)fs.f_type==0x01021994UL||(unsigned long)fs.f_type==0x858458f6UL)&&
        pread(fd,b,sizeof b,0)==sizeof origin-1&&!memcmp(b,origin,sizeof origin-1);
    close(fd);return good?0:-1;
}
int main(int argc,char **argv)
{
    if(argc!=2||strcmp(argv[1],"--apply")||production()){fputs("CONFIG_RESET refused: production NOR/JFFS2 boot required\n",stderr);return 1;}
    int dir=open("/etc/conf/camera",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);char error[160];
    if(dir<0)return 1;
    int rc=camera_config_reset_apply(dir,-1,NULL,NULL,error,sizeof error);close(dir);
    if(rc<0){fprintf(stderr,"CONFIG_RESET failed: %s\n",error);return 1;}
    if(rc)puts("CONFIG_RESET complete: settings cleared; OTA and factory state preserved");
    return 0;
}
#endif
