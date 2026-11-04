#ifndef CAMERA_CONTROL_WIFI_H
#define CAMERA_CONTROL_WIFI_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
struct control_wifi;
struct control_wifi_options {
    const char *active_config; /* existing private root-owned regular file */
    const char *control_socket; /* existing wpa_supplicant AF_UNIX datagram */
    const char *persistent_directory; /* caller-verified mounted private JFFS2 dir */
    pid_t dhcp_pid; /* 0=caller manages DHCP; >1 verified foreground udhcpc wlan0 */
#ifdef CONTROL_WIFI_TEST
    unsigned confirmation_seconds; /* test builds only, production fixed90 */
    int (*renew_test)(pid_t); /* tests only; never invokes host network client */
#endif
};
/* Copies paths; retains checked private directory FDs and a rollback worker.
 * No network modification at init; PING establishes actual control readiness.
 * Context remains alive for service lifetime; no silent destructor rollback. */
int control_wifi_init(struct control_wifi **,const struct control_wifi_options *);
/* SSID1..32 bytes, empty password for an open network; otherwise password
 *8..63 restricted printable ASCII excluding quotes
 * and backslash, or64hex. No credential appears in log or command arguments.
 * One pending transaction; returned ID is for correlation, not authentication.
 * Begin can alter network; failure may leave rollback_error, inspect status. */
int control_wifi_begin(struct control_wifi *,const char *ssid,const char *password,uint64_t *id);
/* Requires current STATUS COMPLETED+requestedSSID; atomically publishes
 * wpa_supplicant.conf in persistent_directory before marking confirmed. */
int control_wifi_confirm(struct control_wifi *,uint64_t id);
/* Queries nonsecret STATUS and exposes pending/remaining and exact errors.
 * Returns0 on formatted JSON even if control STATUS fails (status_error field). */
int control_wifi_status_json(struct control_wifi *,char *,size_t);
/* Explicit cancellation restores active backup, RECONFIGURE+renew. */
/* Start a bounded background driver scan; results include escaped SSIDs. */
int control_wifi_recovery_json(struct control_wifi *,char *,size_t,int include_password);
int control_wifi_recovery_request(struct control_wifi *,unsigned enable);
int control_wifi_scan_begin(struct control_wifi *);
int control_wifi_scan_json(struct control_wifi *,char *,size_t);
int control_wifi_cancel(struct control_wifi *,uint64_t id);
#endif
