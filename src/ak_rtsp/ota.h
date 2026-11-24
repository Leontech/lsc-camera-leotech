#pragma once
/* LEO 2026-11-24: network firmware update (APP partition /dev/mtd7).
 *
 * 1) control port 8091: "OTA ENABLE"  -> opens a 300 s window
 * 2) TCP 8092: client sends "LSCFW <size> <md5hex>\n" followed by exactly <size> bytes
 *    (size must be 5242880 = APP partition, image must start with squashfs magic "hsqs").
 * 3) image is received into /tmp, MD5-verified, current APP is backed up to /tmp,
 *    then a detached /bin/sh script runs flash_tool from /tmp (not from the APP being
 *    overwritten). Success -> reboot. Failure -> previous APP is written back.
 * Replies on the 8092 socket are text lines; final line is OTA_FLASH_OK / OTA_FAILED ... */
#define OTA_PORT        8092
#define OTA_APP_SIZE    5242880
#define OTA_WINDOW_SEC  300

int  ota_start(void);
int  ota_enable(void);           /* returns seconds the window stays open */
int  ota_window_left(void);      /* 0 = closed */
const char *ota_status(void);    /* idle / receiving / flashing / ok / failed: ... */
