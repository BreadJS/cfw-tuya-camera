#ifndef CAMERA_RAM_INSTALL_H
#define CAMERA_RAM_INSTALL_H
#include "ota_linux_mtd.h"
#include <stddef.h>
#include <stdint.h>

struct ram_install_fds { int state,stage,lock,key,progress; };
struct ram_install_result {
    uint64_t sequence;
    /* journal_prepared means known durable success. A journal may also exist
     * after an interrupted prepare that returned failure; it is never removed. */
    unsigned journal_prepared,may_have_changed,verified_written;
    struct ota_apply_progress progress;
};
/* Trusted exec integration only. All FDs are borrowed; private state/stage and
 * the SAME exclusive flock description are revalidated and duplicated before
 * opening MTD. Requires installed NOR-origin PID1 marker and persistent JFFS2
 * config. No caller lease boolean, arbitrary paths, reboot or accepted commit.
 * Failure retains capsule and installation journal. Inspect result even on
 * failure: a mutation may already have completed partially. */
int ram_install_apply_inherited(const struct ram_install_fds *,
                               struct ram_install_result *,char *,size_t);
/* CLI protocol:0 verified success;10 known failure before this attempt could
 * mutate firmware;11 possibly changed or unknown. Never clears any journal. */
int ram_install_exit_status(int outcome,const struct ram_install_result *);
#ifdef CAMERA_INITIAL_DEPLOY
/* Separate console-only initial-deployment build, never the web/runtime
 * helper: recovery-origin and RAM/kernel/process/watchdog/sync checks only.
 * Does not mount, create state, grant a lease or open MTD. */
int ram_install_initial_runtime_check(char *error,size_t error_size);
#endif

#ifdef CAMERA_RAM_INSTALL_TEST
/* Source/fake tests only; never compiled into production. A complete fake
 * syscall table is mandatory so this entry cannot reach physical devices. */
struct ram_install_test_ops {
    void *context;
    int (*environment)(void *,const struct ota_apply_plan *,enum ota_mtd_phase);
    int (*prepare)(void *,struct ota_apply_plan *,const uint8_t[16],char *,size_t);
    const struct ota_mtd_syscalls *syscalls;
    void *syscall_context;
};
int ram_install_test_cmdline(const char *);
int ram_install_test_execute(struct ota_apply_plan *,int progress_directory,
                            const uint8_t boot_id[16],const struct ram_install_test_ops *,
                            struct ram_install_result *,char *,size_t);
#endif
#endif
