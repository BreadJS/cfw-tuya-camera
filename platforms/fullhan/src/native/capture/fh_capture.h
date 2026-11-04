/* Source-owned orchestration, not a vendor userspace ABI. */
#ifndef CAMERHACK_FH_CAPTURE_H
#define CAMERHACK_FH_CAPTURE_H
#include "../vmm/legacy_vmm.h"

#define FH_CAP_VPU_SYS_QUERY 0xc0046942u
#define FH_CAP_VPU_GET_CAPACITY 0xc00c695du
#define FH_CAP_VPU_SYS_INIT 0xc00c6940u
#define FH_CAP_VPU_VI 0xc0f46946u /* encoded244; matching kernel copies12 */
#define FH_CAP_VPU_QUERY 0xc0106943u
#define FH_CAP_VPU_MEMORY 0xc0186944u
#define FH_CAP_VPU_CONFIG 0xc00c6948u
#define FH_CAP_VPU_OPEN 0xc004694fu
#define FH_CAP_VPU_PACE_SET 0xc0086954u
#define FH_CAP_VPU_PACE_GET 0xc0086955u
#define FH_CAP_VPU_ENABLE 0xc004694du
#define FH_CAP_VPU_DISABLE 0xc004694eu /* verified no-op, NOT a stop */
#define FH_CAP_PAE_SYS_QUERY 0xc0045002u
#define FH_CAP_PAE_SYS_INIT 0xc00c5000u
#define FH_CAP_PAE_QUERY 0xc0145003u
#define FH_CAP_PAE_MEMORY 0xc01c5004u
#define FH_CAP_PAE_CONFIG 0xc02c5006u
#define FH_CAP_PAE_RC 0xc054502fu
#define FH_CAP_PAE_START 0xc0045008u
#define FH_CAP_PAE_STOP 0xc0045009u
#define FH_CAP_PAE_IDR 0xc0045014u
#define FH_CAP_MEDIA_BIND 0xc0084d00u
#define FH_CAP_MEDIA_POLL 0xc1704d05u
#define FH_CAP_PAE_RELEASE 0xc0045011u

struct fh_capture_config {
    uint32_t channel; /* Currently0 only; unsupported channel returns-ENOTSUP. */
    uint32_t fps, gop, profile, qp, i_qp, p_qp;
    unsigned experimental_fps30; /* Explicit RAM experiment; default15 remains validated baseline. */
    uint32_t output_width,output_height; /* Actual output; backing/VI capacity stays1080p. */
};
struct fh_capture_io {
    void *opaque;
    /* Exact byte payload length passed independently of _IOC_SIZE. Zero on
     * success; preserves negative errno/vendor errors, no automatic retries. */
    int (*ioctl)(void *, int fd, uint32_t cmd, uint8_t *payload, size_t bytes);
};
enum fh_capture_stage {
    FH_CAP_NONE, FH_CAP_CAPACITY_STAGE, FH_CAP_VPU_SYS_QUERY_STAGE, FH_CAP_VPU_SYS_ALLOC,
    FH_CAP_VPU_SYS_MAP, FH_CAP_VPU_SYS_INIT_STAGE, FH_CAP_VI_STAGE,
    FH_CAP_VPU_QUERY_STAGE, FH_CAP_VPU_ALLOC, FH_CAP_VPU_MAP,
    FH_CAP_VPU_MEMORY_STAGE, FH_CAP_VPU_CONFIG_STAGE, FH_CAP_VPU_OPEN_STAGE,
    FH_CAP_PACE_SET_STAGE, FH_CAP_PACE_GET_STAGE, FH_CAP_VPU_ENABLE_STAGE,
    FH_CAP_PAE_SYS_QUERY_STAGE, FH_CAP_PAE_SYS_ALLOC, FH_CAP_PAE_SYS_MAP,
    FH_CAP_PAE_SYS_INIT_STAGE, FH_CAP_PAE_QUERY_STAGE, FH_CAP_PAE_ALLOC,
    FH_CAP_PAE_MAP, FH_CAP_PAE_MEMORY_STAGE, FH_CAP_PAE_CONFIG_STAGE,
    FH_CAP_RC_STAGE, FH_CAP_START_STAGE, FH_CAP_IDR_STAGE, FH_CAP_BIND_STAGE,
    FH_CAP_STOP_REQUEST_STAGE
};
struct fh_capture_memory {
    size_t handle;
    unsigned retained; /* An allocation exists, including on later failure. */
    uint32_t phys, user, size;
};
struct fh_capture {
    struct fh_capture_config config;
    struct fh_capture_io io;
    struct legacy_vmm *vmm; /* Caller owns and keeps zone open until proven safe. */
    int isp_fd, pae_fd, media_fd;
    enum legacy_vmm_map_method map_method;
    struct fh_capture_memory vpu_sys, vpu_channel, pae_sys, pae_channel;
    enum fh_capture_stage last_completed, uncertain_stage;
    int last_error, last_ioctl_result;
    uint32_t last_command;
    unsigned fault, capacity_verified, vpu_prepared, enable_ack, encoder_prepared;
    unsigned vpu_system_prepared, vpu_memory_prepared, vi_prepared, pae_system_prepared;
    unsigned start_attempted, bind_attempted, stop_attempted;
    unsigned start_ack, idr_ack, bound_ack, stop_ack;
};
void fh_capture_default_config(struct fh_capture_config *);
int fh_capture_validate_config(const struct fh_capture_config *);
/* Fresh storage only. Caller supplies privately owned fds opened under its
 * verified lease: PAE open is destructive. This library never opens/closes
 * device fds or the zone, and never unmaps an allocation. */
int fh_capture_init(struct fh_capture *, const struct fh_capture_config *,
                    const struct fh_capture_io *, struct legacy_vmm *,
                    int isp_fd, int pae_fd, int media_fd,
                    enum legacy_vmm_map_method);
/* Prepare VPU incl channel0/pacing; caller then starts ISP/producers externally. */
int fh_capture_prepare_vpu(struct fh_capture *);
int fh_capture_prepare_vpu_system(struct fh_capture *);
int fh_capture_prepare_vpu_channel_memory(struct fh_capture *);
int fh_capture_prepare_encoder_system(struct fh_capture *);
int fh_capture_prepare_vi(struct fh_capture *);
int fh_capture_finish_vpu(struct fh_capture *);
/* Matching vpu_enable returns0 without work. ACK is not proof of production. */
int fh_capture_enable_vpu(struct fh_capture *);
int fh_capture_prepare_encoder(struct fh_capture *);
/* Requires owner-established producer readiness, not inferred here. */
int fh_capture_start_routing(struct fh_capture *);
int fh_capture_force_idr(struct fh_capture *);
/* Sends verified pae_enc_stop ioctl only. May be used after a fault if start was
 * attempted. ACK is not proof that all DMA/media/ISP is quiescent. No unbind,
 * recycle, VMM unmap/free/close or state reset is performed. */
int fh_capture_request_encoder_stop(struct fh_capture *);
/* Actual Linux backend for already-owned fds. No open or automatic execution. */
int fh_capture_linux_ioctl(void *, int, uint32_t, uint8_t *, size_t);
#endif
