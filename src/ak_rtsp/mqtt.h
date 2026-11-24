#pragma once
/* LEO build13: minimal MQTT 3.1.1 client (QoS 0) with Home Assistant discovery.
 * Config: /etc/config/mqtt.conf (written by the web page on port 80). */
#include <stddef.h>

#define MQTT_CONFIG_FILE "/etc/config/mqtt.conf"

typedef struct {
    int  enabled;
    char host[64];
    int  port;
    char user[64];
    char pass[96];
    char name[48];      /* HA device name */
} mqtt_cfg_t;

int  mqtt_start(void);
void mqtt_get_cfg(mqtt_cfg_t *c);
int  mqtt_set_cfg(const mqtt_cfg_t *c);     /* saves + reconnects */
void mqtt_rediscover(void);                 /* sounds changed -> republish discovery */
void mqtt_status(char *out, size_t cap);    /* human readable */
int  mqtt_connected(void);
const char *mqtt_device_id(void);
const char *net_ip_string(void);
