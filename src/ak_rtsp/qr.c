/* LEO build17: Wi-Fi setup by QR code.
 *
 * Start: reset button held 2-5 s, web (System) or 8091 "QRWIFI".
 * The camera reads frames (sub channel 640x360 + centre crop of the main
 * 1920x1080 channel), decodes QR codes with quirc (ISC licence) and looks for
 * the standard "WIFI:T:WPA;S:<ssid>;P:<pass>;H:<hidden>;;" payload that phones
 * generate when sharing a network.  The new network is written to
 * /etc/config/wpa_supplicant.conf (PSK pre-hashed with PBKDF2, so any
 * character in the password is safe); if no DHCP lease within ~40 s, the
 * previous configuration is restored.  The password is never logged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include "quirc/quirc.h"
#include "speaker.h"
#include "control.h"
#include "ak_rtsp.h"
#include "qr.h"

#define MAIN_W 1920
#define MAIN_H 1080
#define SUB_W  640
#define SUB_H  360
#define CROP_W 960
#define CROP_H 540

static volatile int g_qr_running = 0, g_qr_cancel = 0;
static char         g_qr_status[128] = "idle";

const char *qr_status(void) { return g_qr_status; }
int  qr_running(void) { return g_qr_running; }
void qr_cancel(void) { g_qr_cancel = 1; }

/* ---------------- SHA1 / HMAC / PBKDF2 (WPA PSK) ---------------- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4] << 24 | p[i*4+1] << 16 | p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_init(sha1_ctx *c) { c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE; c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->len=0; c->n=0; }
static void sha1_update(sha1_ctx *c, const void *d, size_t l)
{
    const uint8_t *p = d; c->len += l;
    while (l) { size_t k = 64 - c->n; if (k > l) k = l; memcpy(c->buf + c->n, p, k); c->n += k; p += k; l -= k;
                if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; } }
}
static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8*i));
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) { out[i*4] = c->h[i] >> 24; out[i*4+1] = c->h[i] >> 16; out[i*4+2] = c->h[i] >> 8; out[i*4+3] = c->h[i]; }
}
static void hmac_sha1(const uint8_t *key, size_t kl, const uint8_t *msg, size_t ml, uint8_t out[20])
{
    uint8_t k[64] = { 0 }, ip[64], op[64], ih[20]; sha1_ctx c;
    if (kl > 64) { sha1_init(&c); sha1_update(&c, key, kl); sha1_final(&c, k); } else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    sha1_init(&c); sha1_update(&c, ip, 64); sha1_update(&c, msg, ml); sha1_final(&c, ih);
    sha1_init(&c); sha1_update(&c, op, 64); sha1_update(&c, ih, 20); sha1_final(&c, out);
}
static void wpa_psk(const char *pass, const uint8_t *ssid, size_t sl, uint8_t out[32])
{
    for (int blk = 1; blk <= 2; blk++) {
        uint8_t s[40], u[20], t[20]; memcpy(s, ssid, sl);
        s[sl] = 0; s[sl+1] = 0; s[sl+2] = 0; s[sl+3] = (uint8_t)blk;
        hmac_sha1((const uint8_t *)pass, strlen(pass), s, sl + 4, u); memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) { hmac_sha1((const uint8_t *)pass, strlen(pass), u, 20, u); for (int j = 0; j < 20; j++) t[j] ^= u[j]; }
        memcpy(out + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }
}

/* ---------------- WIFI: payload ---------------- */
typedef struct { char ssid[33]; size_t ssid_len; char pass[64]; char type[8]; int hidden; } wifi_t;

/* reads value up to unescaped ';' */
static size_t take(const char *p, char *out, size_t cap, size_t *consumed)
{
    size_t o = 0, i = 0;
    while (p[i] && p[i] != ';') {
        if (p[i] == '\\' && p[i+1]) i++;
        if (o + 1 < cap) out[o++] = p[i];
        i++;
    }
    out[o] = 0; *consumed = i + (p[i] == ';');
    return o;
}

static int parse_wifi(const char *s, wifi_t *w)
{
    memset(w, 0, sizeof *w); strcpy(w->type, "WPA");
    if (strncasecmp(s, "WIFI:", 5)) return -1;
    const char *p = s + 5;
    while (*p && *p != ';') {
        char key = (char)toupper((unsigned char)p[0]);
        if (p[1] != ':') { while (*p && *p != ';') p++; if (*p) p++; continue; }
        p += 2; size_t used; char tmp[96];
        size_t l = take(p, tmp, sizeof tmp, &used);
        if (key == 'S') { if (l > 32) return -2; memcpy(w->ssid, tmp, l); w->ssid[l] = 0; w->ssid_len = l; }
        else if (key == 'P') { if (l > 63) return -3; snprintf(w->pass, sizeof w->pass, "%s", tmp); }
        else if (key == 'T') snprintf(w->type, sizeof w->type, "%s", tmp);
        else if (key == 'H') w->hidden = !strcasecmp(tmp, "true");
        p += used;
    }
    if (!w->ssid_len) return -4;
    if (!strcasecmp(w->type, "nopass") || !w->type[0]) { w->type[0] = 0; w->pass[0] = 0; }
    else if (strcasecmp(w->type, "WPA") && strcasecmp(w->type, "WPA2") && strcasecmp(w->type, "SAE")) return -5;  /* WEP unsupported */
    else if (strlen(w->pass) < 8) return -6;
    return 0;
}

/* ---------------- apply ---------------- */
#define WPA_CONF "/etc/config/wpa_supplicant.conf"

static int write_conf(const wifi_t *w)
{
    FILE *in = fopen(WPA_CONF, "r"), *out = fopen("/tmp/wpa_new.conf", "w");
    if (!out) { if (in) fclose(in); return -1; }
    int had_ctrl = 0, depth = 0; char line[256];
    if (in) {   /* keep global lines (ctrl_interface, update_config, country...) */
        while (fgets(line, sizeof line, in)) {
            if (strstr(line, "network=") && strchr(line, '{')) { depth = 1; continue; }
            if (depth) { if (strchr(line, '}')) depth = 0; continue; }
            if (strstr(line, "ctrl_interface")) had_ctrl = 1;
            fputs(line, out);
        }
        fclose(in);
    }
    if (!had_ctrl) fputs("ctrl_interface=/var/run/wpa_supplicant\n", out);
    fputs("\nnetwork={\n\tssid=", out);
    for (size_t i = 0; i < w->ssid_len; i++) fprintf(out, "%02x", (uint8_t)w->ssid[i]);
    fputs("\n", out);
    if (w->hidden) fputs("\tscan_ssid=1\n", out);
    if (!w->pass[0]) fputs("\tkey_mgmt=NONE\n", out);
    else {
        uint8_t psk[32];
        if (strlen(w->pass) == 64) { fprintf(out, "\tpsk=%s\n", w->pass); }
        else { wpa_psk(w->pass, (const uint8_t *)w->ssid, w->ssid_len, psk);
               fputs("\tpsk=", out); for (int i = 0; i < 32; i++) fprintf(out, "%02x", psk[i]); fputs("\n", out); }
        fputs("\tkey_mgmt=WPA-PSK\n", out);
    }
    fputs("}\n", out);
    fflush(out); fsync(fileno(out)); fclose(out);
    return 0;
}

static int apply_wifi(const wifi_t *w)
{
    if (write_conf(w)) return -1;
    FILE *s = fopen("/tmp/wifi_apply.sh", "w"); if (!s) return -1;
    fputs("#!/bin/sh\n"
          "R=/tmp/wifi_result; rm -f $R\n"
          "cp " WPA_CONF " /tmp/wpa_old.conf 2>/dev/null\n"
          "cp /tmp/wpa_new.conf " WPA_CONF ".tmp && mv " WPA_CONF ".tmp " WPA_CONF " && sync\n"
          "killall udhcpc 2>/dev/null; killall wpa_supplicant 2>/dev/null; sleep 2\n"
          "wpa_supplicant -Dnl80211 -i wlan0 -c " WPA_CONF " -B\n"
          "if udhcpc -i wlan0 -x hostname:IPCamera -n -q -t 12 -T 3 >/tmp/wifi_dhcp.log 2>&1; then\n"
          "  udhcpc -i wlan0 -x hostname:IPCamera >/dev/null 2>&1 &\n"
          "  echo OK > $R\n"
          "else\n"
          "  killall wpa_supplicant 2>/dev/null; sleep 1\n"
          "  [ -f /tmp/wpa_old.conf ] && cp /tmp/wpa_old.conf " WPA_CONF " && sync\n"
          "  wpa_supplicant -Dnl80211 -i wlan0 -c " WPA_CONF " -B\n"
          "  udhcpc -i wlan0 -x hostname:IPCamera >/dev/null 2>&1 &\n"
          "  echo FAIL > $R\n"
          "fi\n"
          "rm -f /tmp/wpa_new.conf\n", s);
    fclose(s);
    chmod("/tmp/wifi_apply.sh", 0700);
    int rc = system("/bin/sh /tmp/wifi_apply.sh");
    (void)rc;
    FILE *r = fopen("/tmp/wifi_result", "r"); char res[8] = "";
    if (r) { if (!fgets(res, sizeof res, r)) res[0] = 0; fclose(r); }
    return strncmp(res, "OK", 2) == 0 ? 0 : -1;
}

/* ---------------- scan loop ---------------- */
static void *qr_thread(void *arg)
{
    int timeout = (int)(long)arg;
    char led_was[16] = "off"; control_api_get("led.state", led_was, sizeof led_was);
    speaker_stop(); speaker_play_sys("qr_start");
    struct quirc *qs = quirc_new(), *qm = quirc_new();
    if (!qs || !qm || quirc_resize(qs, SUB_W, SUB_H) < 0 || quirc_resize(qm, CROP_W, CROP_H) < 0) {
        snprintf(g_qr_status, sizeof g_qr_status, "error: no memory");
        goto out;
    }
    int main_ok = g_vi_bufs[0] && g_vi_buf_sizes[0] >= (uint32_t)(MAIN_W * MAIN_H);
    printf("[qr] buffers: sub=%p (%u B) main=%p (%u B) -> main crop %s\n", g_vi_sub_bufs[0], g_vi_sub_buf_sizes[0],
           g_vi_bufs[0], g_vi_buf_sizes[0], main_ok ? "used" : "NOT used");
    time_t end = time(NULL) + timeout;
    char last_bad[64] = "";
    int frame = 0, done = 0;
    snprintf(g_qr_status, sizeof g_qr_status, "scanning");
    printf("[qr] scanning for Wi-Fi QR code (%d s)\n", timeout);
    while (!done && !g_qr_cancel && time(NULL) < end) {
        control_api_set("led.state", (frame & 1) ? "on" : "off", 0);   /* blink while scanning */
        int use_main = (frame++ % 2) == 1 && main_ok;
        struct quirc *q = use_main ? qm : qs;
        int w, h; uint8_t *img = quirc_begin(q, &w, &h);
        if (use_main) {
            const uint8_t *y = (const uint8_t *)g_vi_bufs[0] + (MAIN_H - CROP_H) / 2 * MAIN_W + (MAIN_W - CROP_W) / 2;
            for (int r = 0; r < CROP_H; r++) memcpy(img + r * CROP_W, y + r * MAIN_W, CROP_W);
        } else {
            memcpy(img, g_vi_sub_bufs[0], SUB_W * SUB_H);
        }
        quirc_end(q);
        int n = quirc_count(q);
        for (int i = 0; i < n && !done; i++) {
            struct quirc_code code; struct quirc_data data;
            quirc_extract(q, i, &code);
            quirc_decode_error_t de = quirc_decode(&code, &data);
            if (de == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); de = quirc_decode(&code, &data); }
            if (de) continue;
            data.payload[data.payload_len < QUIRC_MAX_PAYLOAD ? data.payload_len : QUIRC_MAX_PAYLOAD - 1] = 0;
            wifi_t wf;
            int pr = parse_wifi((const char *)data.payload, &wf);
            if (pr != 0) {
                char head[64]; snprintf(head, sizeof head, "%.40s", (const char *)data.payload);
                if (strcmp(head, last_bad)) {
                    snprintf(last_bad, sizeof last_bad, "%s", head);
                    printf("[qr] QR without usable Wi-Fi (err %d, %d bytes)\n", pr, data.payload_len);
                    snprintf(g_qr_status, sizeof g_qr_status, "QR bez Wi-Fi (chyba %d)", pr);
                    if (!strncmp(head, "LEOTEST", 7)) { snprintf(g_qr_status, sizeof g_qr_status, "test OK: %.40s", head); speaker_stop(); speaker_play_sys("beep"); done = 1; }
                    else { speaker_stop(); speaker_play_sys("qr_bad"); }
                }
                continue;
            }
            printf("[qr] Wi-Fi QR read: SSID \"%s\" (%s%s) - applying\n", wf.ssid, wf.type[0] ? wf.type : "open", wf.hidden ? ", hidden" : "");
            snprintf(g_qr_status, sizeof g_qr_status, "připojuji k %s", wf.ssid);
            control_api_set("led.state", "on", 0);
            speaker_stop(); speaker_play_sys("qr_read"); sleep(4);
            int ok = apply_wifi(&wf) == 0;
            memset(&wf.pass, 0, sizeof wf.pass);
            printf("[qr] Wi-Fi %s\n", ok ? "connected" : "FAILED - previous network restored");
            snprintf(g_qr_status, sizeof g_qr_status, ok ? "připojeno k %s" : "nezdařilo se (%s), původní síť obnovena", wf.ssid);
            speaker_stop(); speaker_play_sys(ok ? "wifi_ok" : "wifi_fail");
            done = 1;
        }
        usleep(150000);
    }
    if (!done) {
        snprintf(g_qr_status, sizeof g_qr_status, g_qr_cancel ? "zrušeno" : "čas vypršel");
        printf("[qr] %s\n", g_qr_cancel ? "cancelled" : "timeout");
        if (!g_qr_cancel) { speaker_stop(); speaker_play_sys("qr_timeout"); }
    }
out:
    if (qs) quirc_destroy(qs);
    if (qm) quirc_destroy(qm);
    control_api_set("led.state", led_was, 0);
    g_qr_running = 0;
    return NULL;
}

int qr_wifi_start(int timeout_s)
{
    if (g_qr_running) return 1;
    g_qr_running = 1; g_qr_cancel = 0;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, qr_thread, (void *)(long)(timeout_s > 0 ? timeout_s : 120));
    pthread_attr_destroy(&at);
    if (rc != 0) { g_qr_running = 0; return -1; }
    return 0;
}
