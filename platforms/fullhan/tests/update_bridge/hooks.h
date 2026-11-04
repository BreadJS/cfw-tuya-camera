#ifndef CAMERA_UPDATE_TEST_HOOKS_H
#define CAMERA_UPDATE_TEST_HOOKS_H
#include <sys/stat.h>
#include <sys/statfs.h>
#include <pthread.h>
char *camera_test_installer(void);
const char *camera_test_progress(void);
const char *camera_test_mtd_map(void);
int camera_test_fstat(int,struct stat *);
int camera_test_fstatat(int,const char *,struct stat *,int);
int camera_test_fstatfs(int,struct statfs *);
pid_t camera_test_fork(void);
int camera_test_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
void camera_test_sync(void);
int camera_test_reboot(int);
#endif
