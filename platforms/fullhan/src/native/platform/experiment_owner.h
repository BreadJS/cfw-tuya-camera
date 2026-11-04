#ifndef CAMERHACK_EXPERIMENT_OWNER_H
#define CAMERHACK_EXPERIMENT_OWNER_H
#include <sys/types.h>
/* RAM experiment only. The operator first stops stock media and its launchers.
 * This verifies that state and serializes cooperating native owners; it cannot
 * exclude an unrelated privileged program that deliberately ignores the lock. */
struct fh_experiment_owner {
    int lock_fd;
    pid_t pid;
    unsigned references;
    unsigned long long lock_device,lock_inode;
};
int fh_experiment_owner_open(struct fh_experiment_owner *);
int fh_experiment_foreign_users(void);
int fh_experiment_owner_check(void *);
int fh_experiment_owner_retain(void *);
int fh_experiment_owner_release(void *);
/* No runtime shutdown proof is supplied. Retain resources for external reset. */
int fh_experiment_owner_quiesce(void *);
#endif
