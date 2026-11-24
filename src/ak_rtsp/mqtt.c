/* LEO build13: minimal MQTT 3.1.1 client (QoS 0) + Home Assistant discovery.
 *
 * Topics (id = MAC without colons):
 *   lsc/<id>/status                 online/offline (LWT, retained)
 *   lsc/<id>/<key>                  state (retained)
 *   lsc/<id>/<key>/set              command
 *   lsc/<id>/play                   payload = sound name (button / automation)
 *   lsc/<id>/stop, lsc/<id>/restart command
 *   homeassistant/status            "online" -> republish discovery
 */
#include <stdio.h>
#include "leo_time.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "mqtt.h"
#include "control.h"
#include "speaker.h"

const char *build_version_string(void);

static mqtt_cfg_t      g_cfg;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_reconnect = 0, g_rediscover = 0, g_connected = 0;
static char            g_err[96] = "neaktivní";
static char            g_id[16] = "000000000000";
static char            g_ip[20] = "?";
static int             g_sock = -1;
static long long       g_last_tx_ms = 0;   /* last packet actually sent */
static char            g_base[32];

/* published sound buttons (to remove deleted ones) */
#define MAX_SOUNDS 48
static char g_pub_sounds[MAX_SOUNDS][44];
static int  g_pub_count = 0;

/* ---------------- helpers ---------------- */
static long long now_ms(void) { return leo_mono_ms(); }

static void detect_id(void)
{
    DIR *d = opendir("/sys/class/net"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        char p[96], mac[32] = ""; snprintf(p, sizeof p, "/sys/class/net/%s/address", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(mac, sizeof mac, f)) {
            char out[16]; int o = 0;
            for (char *c = mac; *c && o < 12; c++) if (*c != ':' && *c != '\n') out[o++] = *c;
            out[o] = 0;
            if (o == 12 && strcmp(out, "000000000000")) { snprintf(g_id, sizeof g_id, "%s", out); fclose(f); break; }
        }
        fclose(f);
    }
    closedir(d);
    snprintf(g_base, sizeof g_base, "lsc/%s", g_id);
}

const char *mqtt_device_id(void) { return g_id; }

const char *net_ip_string(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        DIR *d = opendir("/sys/class/net");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
            struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
            snprintf(ifr.ifr_name, IFNAMSIZ, "%s", e->d_name);
            if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
                snprintf(g_ip, sizeof g_ip, "%s", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
                break;
            }
        }
        if (d) closedir(d);
        close(s);
    }
    return g_ip;
}

/* ---------------- config ---------------- */
static void cfg_defaults(mqtt_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 0; c->port = 1883;
    snprintf(c->name, sizeof c->name, "LSC kamera");
}

static void cfg_load(void)
{
    mqtt_cfg_t c; cfg_defaults(&c);
    FILE *f = fopen(MQTT_CONFIG_FILE, "r");
    if (f) {
        char line[200];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *sp = strchr(line, ' '); if (!sp) continue;
            *sp = 0; const char *v = sp + 1;
            if      (!strcmp(line, "enabled")) c.enabled = atoi(v);
            else if (!strcmp(line, "host"))    snprintf(c.host, sizeof c.host, "%s", v);
            else if (!strcmp(line, "port"))    c.port = atoi(v);
            else if (!strcmp(line, "user"))    snprintf(c.user, sizeof c.user, "%s", v);
            else if (!strcmp(line, "pass"))    snprintf(c.pass, sizeof c.pass, "%s", v);
            else if (!strcmp(line, "name"))    snprintf(c.name, sizeof c.name, "%s", v);
        }
        fclose(f);
    }
    if (c.port <= 0 || c.port > 65535) c.port = 1883;
    pthread_mutex_lock(&g_cfg_lock); g_cfg = c; pthread_mutex_unlock(&g_cfg_lock);
}

void mqtt_get_cfg(mqtt_cfg_t *c)
{
    pthread_mutex_lock(&g_cfg_lock); *c = g_cfg; pthread_mutex_unlock(&g_cfg_lock);
}

int mqtt_set_cfg(const mqtt_cfg_t *c)
{
    const char *tmp = MQTT_CONFIG_FILE ".tmp";
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    fprintf(f, "enabled %d\nhost %s\nport %d\nuser %s\npass %s\nname %s\n",
            c->enabled, c->host, c->port, c->user, c->pass, c->name);
    fflush(f); fsync(fileno(f)); fclose(f);
    if (rename(tmp, MQTT_CONFIG_FILE) != 0) { unlink(tmp); return -1; }
    sync();
    pthread_mutex_lock(&g_cfg_lock); g_cfg = *c; pthread_mutex_unlock(&g_cfg_lock);
    g_reconnect = 1;
    return 0;
}

void mqtt_rediscover(void) { g_rediscover = 1; }
int  mqtt_connected(void)  { return g_connected; }
void mqtt_status(char *out, size_t cap)
{
    snprintf(out, cap, "%s", g_connected ? "připojeno" : g_err);
}

/* ---------------- MQTT wire ---------------- */
static int send_all(const void *b, size_t n)
{
    g_last_tx_ms = leo_mono_ms();
    const char *p = b;
    while (n) {
        ssize_t w = send(g_sock, p, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static size_t put_len(unsigned char *b, size_t len)
{
    size_t i = 0;
    do { unsigned char d = len % 128; len /= 128; if (len) d |= 0x80; b[i++] = d; } while (len);
    return i;
}

static size_t put_str(unsigned char *b, const char *s)
{
    size_t l = strlen(s);
    b[0] = (unsigned char)(l >> 8); b[1] = (unsigned char)l; memcpy(b + 2, s, l);
    return l + 2;
}

static int mq_packet(unsigned char type, const unsigned char *var, size_t vlen)
{
    unsigned char hdr[5]; hdr[0] = type;
    size_t h = 1 + put_len(hdr + 1, vlen);
    if (send_all(hdr, h)) return -1;
    return vlen ? send_all(var, vlen) : 0;
}

static int mq_publish(const char *topic, const char *payload, int retain)
{
    size_t tl = strlen(topic), pl = strlen(payload);
    unsigned char *b = malloc(tl + pl + 2);
    if (!b) return -1;
    size_t o = put_str(b, topic);
    memcpy(b + o, payload, pl); o += pl;
    int r = mq_packet(0x30 | (retain ? 1 : 0), b, o);
    free(b);
    return r;
}

static int mq_subscribe(unsigned short pid, const char *topic)
{
    unsigned char b[160]; size_t o = 0;
    b[o++] = pid >> 8; b[o++] = pid & 255;
    o += put_str(b + o, topic);
    b[o++] = 0;
    return mq_packet(0x82, b, o);
}

static int recv_full(void *buf, size_t n, int timeout_ms)
{
    char *p = buf;
    long long end = now_ms() + timeout_ms;
    while (n) {
        int left = (int)(end - now_ms()); if (left <= 0) return -1;
        fd_set rs; FD_ZERO(&rs); FD_SET(g_sock, &rs);
        struct timeval tv = { left / 1000, (left % 1000) * 1000 };
        int s = select(g_sock + 1, &rs, NULL, NULL, &tv);
        if (s <= 0) { if (s < 0 && errno == EINTR) continue; return -1; }
        ssize_t r = recv(g_sock, p, n, 0);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* reads one packet; returns type or -1. payload malloc'd into *out */
static int mq_read(unsigned char **out, size_t *olen, int timeout_ms)
{
    unsigned char t;
    if (recv_full(&t, 1, timeout_ms)) return -1;
    size_t len = 0, mul = 1; unsigned char d;
    for (int i = 0; i < 4; i++) {
        if (recv_full(&d, 1, 5000)) return -1;
        len += (d & 127) * mul; mul *= 128;
        if (!(d & 128)) break;
    }
    if (len > 65536) return -1;
    unsigned char *b = malloc(len + 1);
    if (!b) return -1;
    if (len && recv_full(b, len, 10000)) { free(b); return -1; }
    b[len] = 0;
    *out = b; *olen = len;
    return t;
}

/* ---------------- HA mapping ---------------- */
static const char *map_to_cz(const char *key, const char *v)
{
    if (!strcmp(key, "rezim")) return !strcmp(v, "day") ? "den" : !strcmp(v, "night") ? "noc" : "auto";
    if (!strcmp(key, "ir"))    return !strcmp(v, "on") ? "zap" : !strcmp(v, "off") ? "vyp" : "auto";
    if (!strcmp(key, "stav"))  return !strcmp(v, "night") ? "noc" : "den";
    if (!strcmp(key, "led") || !strcmp(key, "flip") || !strcmp(key, "mirror") || !strcmp(key, "ir_stav"))
        return !strcmp(v, "on") ? "ON" : "OFF";
    return v;
}

typedef struct { const char *key, *param; } state_map_t;
static const state_map_t g_states[] = {
    { "rezim",   "night.mode" }, { "stav",   "night.state" },
    { "ir",      "ir.led" },     { "ir_stav", "ir.state" },
    { "led",     "led.state" },  { "flip",   "image.flip" },
    { "mirror",  "image.mirror" },{ "volume", "audio.volume" },
    { "jas",     "isp.brightness" }, { "kontrast", "isp.contrast" },
    { "sytost",  "isp.saturation" }, { "ostrost",  "isp.sharpness" },
};
#define N_STATES (sizeof g_states / sizeof g_states[0])
static char g_last[N_STATES][24];

static void publish_states(int force)
{
    char v[64], topic[64];
    for (size_t i = 0; i < N_STATES; i++) {
        if (control_api_get(g_states[i].param, v, sizeof v) != 0) continue;
        const char *cz = map_to_cz(g_states[i].key, v);
        if (!force && !strcmp(g_last[i], cz)) continue;
        snprintf(topic, sizeof topic, "%s/%s", g_base, g_states[i].key);
        if (mq_publish(topic, cz, 1) == 0) snprintf(g_last[i], sizeof g_last[i], "%s", cz);
    }
}

static void disc(const char *comp, const char *obj, const char *extra)
{
    mqtt_cfg_t c; mqtt_get_cfg(&c);
    char topic[128], *p = malloc(1400);
    if (!p) return;
    snprintf(topic, sizeof topic, "homeassistant/%s/lsc_%s/%s/config", comp, g_id, obj);
    snprintf(p, 1400,
        "{\"uniq_id\":\"lsc_%s_%s\",\"obj_id\":\"lsc_%s_%s\",\"avty_t\":\"%s/status\",%s,"
        "\"dev\":{\"ids\":[\"lsc_%s\"],\"name\":\"%s\",\"mf\":\"LSC / leotech.cz\","
        "\"mdl\":\"LSC Smart Connect 3215672 (ak_rtsp)\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}}",
        g_id, obj, g_id, obj, g_base, extra, g_id, c.name, build_version_string(), g_ip);
    mq_publish(topic, p, 1);
    free(p);
}

static void publish_discovery(void)
{
    char e[640];
#define ST(k)  "\"stat_t\":\"%s/" k "\""
#define CMD(k) "\"cmd_t\":\"%s/" k "/set\""
    snprintf(e, sizeof e, "\"name\":\"Režim den/noc\",\"ic\":\"mdi:theme-light-dark\"," ST("rezim") "," CMD("rezim") ",\"ops\":[\"auto\",\"den\",\"noc\"]", g_base, g_base);
    disc("select", "rezim", e);
    snprintf(e, sizeof e, "\"name\":\"IR přísvit\",\"ic\":\"mdi:led-on\"," ST("ir") "," CMD("ir") ",\"ops\":[\"auto\",\"zap\",\"vyp\"]", g_base, g_base);
    disc("select", "ir", e);
    snprintf(e, sizeof e, "\"name\":\"Stavová LED\",\"ic\":\"mdi:led-variant-on\"," ST("led") "," CMD("led"), g_base, g_base);
    disc("switch", "led", e);
    snprintf(e, sizeof e, "\"name\":\"Otočit obraz\",\"ic\":\"mdi:flip-vertical\"," ST("flip") "," CMD("flip"), g_base, g_base);
    disc("switch", "flip", e);
    snprintf(e, sizeof e, "\"name\":\"Zrcadlit obraz\",\"ic\":\"mdi:flip-horizontal\"," ST("mirror") "," CMD("mirror"), g_base, g_base);
    disc("switch", "mirror", e);
    snprintf(e, sizeof e, "\"name\":\"Hlasitost\",\"ic\":\"mdi:volume-high\"," ST("volume") "," CMD("volume") ",\"min\":0,\"max\":200,\"step\":5,\"unit_of_meas\":\"%%\",\"mode\":\"slider\"", g_base, g_base);
    disc("number", "volume", e);
    static const char *isp[4][2] = { {"jas","Jas"}, {"kontrast","Kontrast"}, {"sytost","Sytost barev"}, {"ostrost","Ostrost"} };
    for (int i = 0; i < 4; i++) {
        snprintf(e, sizeof e, "\"name\":\"%s\",\"ent_cat\":\"config\",\"stat_t\":\"%s/%s\",\"cmd_t\":\"%s/%s/set\",\"min\":-50,\"max\":50,\"step\":1,\"mode\":\"slider\"",
                 isp[i][1], g_base, isp[i][0], g_base, isp[i][0]);
        disc("number", isp[i][0], e);
    }
    snprintf(e, sizeof e, "\"name\":\"Den/noc\",\"ic\":\"mdi:weather-night\"," ST("stav"), g_base);
    disc("sensor", "stav", e);
    snprintf(e, sizeof e, "\"name\":\"IR přísvit svítí\",\"ic\":\"mdi:lightbulb-night\",\"dev_cla\":\"light\"," ST("ir_stav"), g_base);
    disc("binary_sensor", "ir_stav", e);
    snprintf(e, sizeof e, "\"name\":\"Firmware\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:chip\"," ST("fw"), g_base);
    disc("sensor", "fw", e);
    snprintf(e, sizeof e, "\"name\":\"IP adresa\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:ip-network\"," ST("ip"), g_base);
    disc("sensor", "ip", e);
    snprintf(e, sizeof e, "\"name\":\"Zastavit zvuk\",\"ic\":\"mdi:stop\",\"cmd_t\":\"%s/stop\"", g_base);
    disc("button", "stop", e);
    snprintf(e, sizeof e, "\"name\":\"Restart kamery\",\"dev_cla\":\"restart\",\"ent_cat\":\"config\",\"cmd_t\":\"%s/restart\"", g_base);
    disc("button", "restart", e);

    /* one button per sound (built-in + user) */
    char list[1024], ulist[1024], names[MAX_SOUNDS][44]; int n = 0;
    speaker_list(list, sizeof list);
    speaker_list_user(ulist, sizeof ulist);
    for (char *t = strtok(list, " "); t && n < MAX_SOUNDS; t = strtok(NULL, " ")) snprintf(names[n++], 44, "%s", t);
    for (char *t = strtok(ulist, " "); t && n < MAX_SOUNDS; t = strtok(NULL, " ")) {
        char *c = strchr(t, ':'); if (c) *c = 0;
        snprintf(names[n++], 44, "%s", t);
    }
    for (int i = 0; i < n; i++) {
        char obj[56]; snprintf(obj, sizeof obj, "zvuk_%s", names[i]);
        snprintf(e, sizeof e, "\"name\":\"Zvuk %s\",\"ic\":\"mdi:bullhorn\",\"cmd_t\":\"%s/play\",\"pl_prs\":\"%s\"", names[i], g_base, names[i]);
        disc("button", obj, e);
    }
    /* remove buttons of deleted sounds */
    for (int i = 0; i < g_pub_count; i++) {
        int found = 0;
        for (int j = 0; j < n; j++) if (!strcmp(g_pub_sounds[i], names[j])) { found = 1; break; }
        if (!found) {
            char topic[128]; snprintf(topic, sizeof topic, "homeassistant/button/lsc_%s/zvuk_%s/config", g_id, g_pub_sounds[i]);
            mq_publish(topic, "", 1);
        }
    }
    for (int i = 0; i < n; i++) snprintf(g_pub_sounds[i], 44, "%s", names[i]);
    g_pub_count = n;

    char t[64];
    snprintf(t, sizeof t, "%s/fw", g_base); mq_publish(t, build_version_string(), 1);
    snprintf(t, sizeof t, "%s/ip", g_base); mq_publish(t, net_ip_string(), 1);
    publish_states(1);
#undef ST
#undef CMD
}

/* ---------------- incoming ---------------- */
static void handle_cmd(const char *topic, const char *payload)
{
    size_t bl = strlen(g_base);
    if (!strcmp(topic, "homeassistant/status")) {
        if (!strcmp(payload, "online")) g_rediscover = 1;
        return;
    }
    if (strncmp(topic, g_base, bl) || topic[bl] != '/') return;
    const char *k = topic + bl + 1;
    printf("[mqtt] cmd %s = %s\n", k, payload);
    if (!strcmp(k, "play"))    { speaker_play(payload); return; }
    if (!strcmp(k, "stop"))    { speaker_stop(); return; }
    if (!strcmp(k, "restart")) {
        char w[64]; snprintf(w, sizeof w, "%s/status", g_base); mq_publish(w, "offline", 1);
        printf("[mqtt] restart requested\n"); sync(); sleep(1); reboot(RB_AUTOBOOT); return;
    }

    char key[24]; const char *sl = strchr(k, '/');
    if (!sl || strcmp(sl, "/set") || (size_t)(sl - k) >= sizeof key) return;
    memcpy(key, k, sl - k); key[sl - k] = 0;
    const char *param = NULL, *val = payload;
    for (size_t i = 0; i < N_STATES; i++) if (!strcmp(g_states[i].key, key)) param = g_states[i].param;
    if (!param || !strcmp(key, "stav") || !strcmp(key, "ir_stav")) return;
    if (!strcmp(key, "rezim")) val = !strcmp(payload, "den") ? "day" : !strcmp(payload, "noc") ? "night" : "auto";
    else if (!strcmp(key, "ir")) val = !strcmp(payload, "zap") ? "on" : !strcmp(payload, "vyp") ? "off" : "auto";
    else if (!strcmp(key, "led") || !strcmp(key, "flip") || !strcmp(key, "mirror")) val = !strcmp(payload, "ON") ? "on" : "off";
    char num[16];
    if (!strcmp(key, "volume") || !strcmp(key, "jas") || !strcmp(key, "kontrast") || !strcmp(key, "sytost") || !strcmp(key, "ostrost")) {
        snprintf(num, sizeof num, "%d", (int)atof(payload)); val = num;
    }
    control_api_set(param, val, 1);
    publish_states(0);
}

/* ---------------- main loop ---------------- */
static int mq_connect(const mqtt_cfg_t *c)
{
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char port[8]; snprintf(port, sizeof port, "%d", c->port);
    if (getaddrinfo(c->host, port, &hints, &res) != 0 || !res) { snprintf(g_err, sizeof g_err, "nelze přeložit %s", c->host); return -1; }
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 5, 0 };
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(g_sock, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(g_err, sizeof g_err, "broker nedostupný (%s)", strerror(errno));
        freeaddrinfo(res); close(g_sock); g_sock = -1; return -1;
    }
    freeaddrinfo(res);
    int one = 1; setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    char cid[32], will[64]; snprintf(cid, sizeof cid, "lsc_%s", g_id); snprintf(will, sizeof will, "%s/status", g_base);
    unsigned char b[512]; size_t o = 0;
    o += put_str(b + o, "MQTT"); b[o++] = 4;
    unsigned char flags = 0x02 | 0x04 | 0x20;           /* clean, will, will retain */
    if (c->user[0]) flags |= 0x80;
    if (c->pass[0]) flags |= 0x40;
    b[o++] = flags; b[o++] = 0; b[o++] = 60;             /* keepalive 60 s */
    o += put_str(b + o, cid);
    o += put_str(b + o, will);
    o += put_str(b + o, "offline");
    if (c->user[0]) o += put_str(b + o, c->user);
    if (c->pass[0]) o += put_str(b + o, c->pass);
    if (mq_packet(0x10, b, o)) goto fail;
    unsigned char *pl = NULL; size_t pll = 0;
    int t = mq_read(&pl, &pll, 8000);
    if (t != 0x20 || pll < 2) { free(pl); snprintf(g_err, sizeof g_err, "broker neodpověděl"); goto fail2; }
    int rc = pl[1]; free(pl);
    if (rc) {
        snprintf(g_err, sizeof g_err, rc == 4 || rc == 5 ? "špatné jméno/heslo (rc %d)" : "odmítnuto (rc %d)", rc);
        goto fail2;
    }
    mq_publish(will, "online", 1);
    char sub[64];
    snprintf(sub, sizeof sub, "%s/+/set", g_base); mq_subscribe(1, sub);
    snprintf(sub, sizeof sub, "%s/play", g_base);  mq_subscribe(2, sub);
    snprintf(sub, sizeof sub, "%s/stop", g_base);  mq_subscribe(3, sub);
    snprintf(sub, sizeof sub, "%s/restart", g_base); mq_subscribe(4, sub);
    mq_subscribe(5, "homeassistant/status");
    return 0;
fail:
    snprintf(g_err, sizeof g_err, "chyba odesílání");
fail2:
    close(g_sock); g_sock = -1;
    return -1;
}

static void *mqtt_loop(void *arg)
{
    (void)arg;
    int backoff = 5;
    for (;;) {
        mqtt_cfg_t c; mqtt_get_cfg(&c);
        g_reconnect = 0;
        if (!c.enabled || !c.host[0]) {
            snprintf(g_err, sizeof g_err, c.enabled ? "není zadán server" : "vypnuto");
            while (!g_reconnect) sleep(1);
            continue;
        }
        net_ip_string();
        if (mq_connect(&c) != 0) {
            printf("[mqtt] connect %s:%d failed: %s\n", c.host, c.port, g_err);
            for (int i = 0; i < backoff && !g_reconnect; i++) sleep(1);
            if (backoff < 60) backoff *= 2;
            continue;
        }
        backoff = 5;
        g_connected = 1;
        printf("[mqtt] connected to %s:%d as lsc_%s\n", c.host, c.port, g_id);
        memset(g_last, 0, sizeof g_last);
        publish_discovery();
        long long last_tx = now_ms(), last_state = now_ms(), last_full = now_ms();
        for (;;) {
            if (g_reconnect) break;
            if (g_rediscover) { g_rediscover = 0; publish_discovery(); }
            unsigned char *pl = NULL; size_t pll = 0;
            int t = mq_read(&pl, &pll, 1000);
            if (t < 0) {
                /* timeout or error: distinguish via socket probe */
                char x; ssize_t r = recv(g_sock, &x, 1, MSG_PEEK | MSG_DONTWAIT);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { snprintf(g_err, sizeof g_err, "spojení přerušeno"); break; }
            } else if ((t & 0xF0) == 0x30 && pll >= 2) {
                size_t tl = (size_t)pl[0] << 8 | pl[1];
                size_t off = 2 + tl + (((t >> 1) & 3) ? 2 : 0);
                if (off <= pll) {
                    char topic[128]; size_t cl = tl < sizeof topic - 1 ? tl : sizeof topic - 1;
                    memcpy(topic, pl + 2, cl); topic[cl] = 0;
                    handle_cmd(topic, (const char *)pl + off);
                    last_state = 0;
                }
            }
            free(pl);
            long long now = now_ms();
            if (now - last_state > 5000) {
                int full = now - last_full > 300000;
                publish_states(full); last_state = now;
                if (full) last_full = now;
            }
            if (now - g_last_tx_ms > 30000) { if (mq_packet(0xC0, NULL, 0)) { snprintf(g_err, sizeof g_err, "ping selhal"); break; } }
            (void)last_tx;
        }
        if (g_sock >= 0) {
            if (g_reconnect) { char w[64]; snprintf(w, sizeof w, "%s/status", g_base); mq_publish(w, "offline", 1); mq_packet(0xE0, NULL, 0); }
            close(g_sock); g_sock = -1;
        }
        g_connected = 0;
        printf("[mqtt] disconnected: %s\n", g_err);
        sleep(1);
    }
    return NULL;
}

int mqtt_start(void)
{
    signal(SIGPIPE, SIG_IGN);
    detect_id();
    cfg_load();
    pthread_t t;
    if (pthread_create(&t, NULL, mqtt_loop, NULL) != 0) return -1;
    pthread_detach(t);
    printf("[mqtt] started, device id %s\n", g_id);
    return 0;
}
