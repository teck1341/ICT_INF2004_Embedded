/*
 * Copy this file to local/wifi_credentials.h and edit the copy.
 * scripts/dev.sh copies local/wifi_credentials.h into the generated port
 * checkout as config/wifi_credentials.h for the MQTT build profile.
 * The real file is git-ignored and must never be committed.
 *
 * This mirrors config/wifi_credentials.example.h in the professor's port
 * (sirfonzie/mtk3smp-rp2040 at the pinned commit).
 */
#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

/* Fill in the values below, then change the 0 on the next line to 1. */
#define UTK_WIFI_CREDENTIALS_SET   0

#if !UTK_WIFI_CREDENTIALS_SET
#error "config/wifi_credentials.h: placeholder values. Set your network SSID and password, then set UTK_WIFI_CREDENTIALS_SET to 1."
#endif

#define UTK_WIFI_SSID       "replace-with-ssid"
#define UTK_WIFI_PASSWORD   "replace-with-password"
#define UTK_WIFI_AUTH       CYW43_AUTH_WPA2_AES_PSK

#endif /* WIFI_CREDENTIALS_H */
