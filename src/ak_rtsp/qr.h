#pragma once
/* LEO build17: Wi-Fi setup by QR code (see qr.c) */
int         qr_wifi_start(int timeout_s);   /* 0 started, 1 already running, -1 error */
void        qr_cancel(void);
int         qr_running(void);
const char *qr_status(void);
