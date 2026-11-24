#pragma once
/* LEO build13: web configuration page (port 80) + reset button */
#define WEB_PORT        80
#define WEB_CONFIG_FILE "/etc/config/web.conf"
int web_start(void);
int web_reset_password(void);
int button_start(void);   /* button.c: reset button actions */
