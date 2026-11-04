#ifndef CAMERHACK_FH_CAPACITY_H
#define CAMERHACK_FH_CAPACITY_H
#include <stddef.h>
#include <stdint.h>
struct fh_capture;
struct fh_capacity_io {
    void *opaque;
    /* Verifies already-held exclusive whole-media/zone ownership; no default
     * success or unchecked owner boolean. Called immediately before writing. */
    int (*verify_lease)(void *);
    ptrdiff_t (*write)(void *,const char *,size_t);
};
struct fh_capacity {
    struct fh_capacity_io io;
    unsigned attempted, written, fault, readback;
    uint32_t actual_width, actual_height;
};
int fh_capacity_init(struct fh_capacity *,const struct fh_capacity_io *);
/* Invoke before opening/initializing media SYS/VPU; channel0 only. */
int fh_capacity_prepare(struct fh_capacity *);
/* Invoke on private ISP fd before ANY VPU memory query/init. */
int fh_capture_verify_capacity(struct fh_capture *,struct fh_capacity *);
struct fh_capacity_posix {
    void *lease_opaque;
    int (*verify_lease)(void *);
    int retained_fd; /* A close error must not drop recovery diagnostics. */
    int last_error, last_close_error;
    unsigned quarantine;
};
int fh_capacity_posix_init(struct fh_capacity_posix *,void *,int (*verify_lease)(void *));
int fh_capacity_posix_verify(void *);
ptrdiff_t fh_capacity_posix_write(void *,const char *,size_t);
#endif
