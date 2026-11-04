#ifndef CAMERA_WIFI_RADIO_H
#define CAMERA_WIFI_RADIO_H
/* Creates a separate AP virtual interface on wlan0's radio. Never changes
 * wlan0's interface type. Existing names are rejected, not adopted. */
int camera_wifi_ap_create(void);
int camera_wifi_ap_delete(void);
int camera_wifi_ap_address(void);
int camera_wifi_station_ready(void);
#endif
