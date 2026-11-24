/* LEO build13: web configuration page on port 80 (leotech.cz).
 *
 * Auth: password (salted MD5) in /etc/config/web.conf, session cookie.
 * No password set -> page shows first-time setup.  Reset button held 5 s
 * clears the password (button.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "web.h"
#include "mqtt.h"
#include "control.h"
#include "speaker.h"
#include "ota.h"
#include "md5.h"
#include "qr.h"
#include "ak_rtsp.h"
#include "web_html.h"

const char *build_version_string(void);

#define MAX_SESS 6
static char            g_sess[MAX_SESS][33];
static int             g_sess_next = 0;
static pthread_mutex_t g_web_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_fail_count = 0;
static time_t          g_fail_until = 0;

/* ---------------- password ---------------- */
static void md5hex(const char *s, char out[33])
{
    md5_ctx c; uint8_t d[16];
    md5_init(&c); md5_update(&c, s, strlen(s)); md5_final(&c, d);
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static void rand_hex(char *out, int bytes)
{
    unsigned char b[32]; int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, bytes) != bytes) { for (int i = 0; i < bytes; i++) b[i] = rand(); }
    if (fd >= 0) close(fd);
    for (int i = 0; i < bytes; i++) sprintf(out + i * 2, "%02x", b[i]);
}

static int pass_load(char salt[33], char hash[33])
{
    salt[0] = hash[0] = 0;
    FILE *f = fopen(WEB_CONFIG_FILE, "r"); if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "salt ", 5)) snprintf(salt, 33, "%s", line + 5);
        if (!strncmp(line, "hash ", 5)) snprintf(hash, 33, "%s", line + 5);
    }
    fclose(f);
    return (salt[0] && hash[0]) ? 0 : -1;
}

/* 0 = not configured (first-time setup), 1 = password, 2 = no password */
static int auth_mode(void)
{
    char s[33], h[33];
    if (pass_load(s, h) == 0) return 1;
    FILE *f = fopen(WEB_CONFIG_FILE, "r"); if (!f) return 0;
    char line[64]; int none = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "nopass 1", 8)) none = 1;
    fclose(f);
    return none ? 2 : 0;
}

static int pass_set_none(void)
{
    const char *tmp = WEB_CONFIG_FILE ".tmp";
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    fprintf(f, "nopass 1\n"); fflush(f); fsync(fileno(f)); fclose(f);
    if (rename(tmp, WEB_CONFIG_FILE)) { unlink(tmp); return -1; }
    sync();
    return 0;
}

static int pass_check(const char *pw)
{
    char s[33], h[33], buf[200], calc[33];
    if (pass_load(s, h)) return 0;
    snprintf(buf, sizeof buf, "%s:%s", s, pw); md5hex(buf, calc);
    return !strcmp(calc, h);
}

static int pass_store(const char *pw)
{
    char s[33], buf[200], h[33];
    rand_hex(s, 16);
    snprintf(buf, sizeof buf, "%s:%s", s, pw); md5hex(buf, h);
    const char *tmp = WEB_CONFIG_FILE ".tmp";
    FILE *f = fopen(tmp, "w"); if (!f) return -1;
    fprintf(f, "salt %s\nhash %s\n", s, h);
    fflush(f); fsync(fileno(f)); fclose(f);
    if (rename(tmp, WEB_CONFIG_FILE)) { unlink(tmp); return -1; }
    sync();
    pthread_mutex_lock(&g_web_lock); memset(g_sess, 0, sizeof g_sess); pthread_mutex_unlock(&g_web_lock);
    return 0;
}

int web_reset_password(void)
{
    unlink(WEB_CONFIG_FILE); sync();
    pthread_mutex_lock(&g_web_lock); memset(g_sess, 0, sizeof g_sess); pthread_mutex_unlock(&g_web_lock);
    printf("[web] password cleared\n");
    return 0;
}

static const char *sess_new(void)
{
    static char tok[33];
    pthread_mutex_lock(&g_web_lock);
    rand_hex(g_sess[g_sess_next], 16);
    snprintf(tok, sizeof tok, "%s", g_sess[g_sess_next]);
    g_sess_next = (g_sess_next + 1) % MAX_SESS;
    pthread_mutex_unlock(&g_web_lock);
    return tok;
}

static int sess_ok(const char *tok)
{
    if (!tok || strlen(tok) != 32) return 0;
    int ok = 0;
    pthread_mutex_lock(&g_web_lock);
    for (int i = 0; i < MAX_SESS; i++) if (g_sess[i][0] && !strcmp(g_sess[i], tok)) ok = 1;
    pthread_mutex_unlock(&g_web_lock);
    return ok;
}

static void sess_drop(const char *tok)
{
    pthread_mutex_lock(&g_web_lock);
    for (int i = 0; i < MAX_SESS; i++) if (tok && !strcmp(g_sess[i], tok)) g_sess[i][0] = 0;
    pthread_mutex_unlock(&g_web_lock);
}

/* ---------------- HTTP helpers ---------------- */
typedef struct {
    int fd;
    char method[8], path[96], query[256], cookie_sid[40];
    long clen;
} req_t;

static int wall(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; } p += w; n -= w; }
    return 0;
}

static void respond(int fd, int code, const char *ctype, const char *extra_hdr, const char *body, size_t blen)
{
    char h[512];
    const char *st = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : code == 413 ? "Too Large" : code == 429 ? "Too Many" : "Error";
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "X-Frame-Options: DENY\r\nConnection: close\r\n%s\r\n", code, st, ctype, blen, extra_hdr ? extra_hdr : "");
    wall(fd, h, n);
    if (blen) wall(fd, body, blen);
}

static void json(int fd, int code, const char *body) { respond(fd, code, "application/json; charset=utf-8", NULL, body, strlen(body)); }
static void jerr2(int fd, int code, const char *ec, const char *msg) { char b[320]; snprintf(b, sizeof b, "{\"ok\":0,\"code\":\"%s\",\"err\":\"%s\"}", ec, msg); json(fd, code, b); }
#define jerr(fd, code, msg) jerr2(fd, code, "", msg)
static void jok(int fd) { json(fd, 200, "{\"ok\":1}"); }

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* get urlencoded field from "a=1&b=2" */
static int form_get(const char *src, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = src;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1; size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '+') { out[o++] = ' '; p++; }
                else if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { out[o++] = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 3; }
                else out[o++] = *p++;
            }
            out[o] = 0; return 0;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    out[0] = 0; return -1;
}

static void json_esc(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = 0;
}

static int read_head(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, buf + n, 1, 0);
        if (r <= 0) return -1;
        n++;
        if (n >= 4 && !memcmp(buf + n - 4, "\r\n\r\n", 4)) { buf[n] = 0; return (int)n; }
    }
    return -1;
}

static char *read_body(int fd, long len, long max)
{
    if (len < 0 || len > max) return NULL;
    char *b = malloc(len + 1); if (!b) return NULL;
    long got = 0;
    while (got < len) { ssize_t r = recv(fd, b + got, len - got, 0); if (r <= 0) { free(b); return NULL; } got += r; }
    b[len] = 0;
    return b;
}

/* ---------------- state JSON ---------------- */
static void state_json(int fd)
{
    static const char *params[] = { "night.mode","night.state","ir.led","ir.state","led.state","image.flip","image.mirror",
        "audio.volume","isp.brightness","isp.contrast","isp.saturation","isp.sharpness","ae.enabled","ae.exp_max",
        "night.trigger_hw_exp","night.day_hw_exp","config.saved","ota.status" };
    size_t cap = 8192; char *b = malloc(cap); if (!b) { jerr(fd, 500, "mem"); return; }
    size_t o = 0; char v[96], e[200];
    struct sysinfo si; sysinfo(&si);
    json_esc(build_version_string(), e, sizeof e);
    o += snprintf(b + o, cap - o, "{\"ok\":1,\"auth\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"id\":\"%s\",\"uptime\":%ld,\"mem_free_kb\":%lu,\"p\":{",
                  auth_mode() == 1 ? "pass" : "none", e, net_ip_string(), mqtt_device_id(), si.uptime, (unsigned long)(si.freeram / 1024));
    for (size_t i = 0; i < sizeof params / sizeof *params; i++) {
        control_api_get(params[i], v, sizeof v); json_esc(v, e, sizeof e);
        o += snprintf(b + o, cap - o, "%s\"%s\":\"%s\"", i ? "," : "", params[i], e);
    }
    mqtt_cfg_t c; mqtt_get_cfg(&c);
    char h[140], u[140], n[100], st[120];
    json_esc(c.host, h, sizeof h); json_esc(c.user, u, sizeof u); json_esc(c.name, n, sizeof n);
    mqtt_status(v, sizeof v); json_esc(v, st, sizeof st);
    o += snprintf(b + o, cap - o, "},\"mqtt\":{\"enabled\":%d,\"host\":\"%s\",\"port\":%d,\"user\":\"%s\",\"has_pass\":%d,\"name\":\"%s\",\"connected\":%d,\"status\":\"%s\",\"base\":\"lsc/%s\"},",
                  c.enabled, h, c.port, u, c.pass[0] ? 1 : 0, n, mqtt_connected(), st, mqtt_device_id());
    char list[1024], ulist[1024];
    speaker_list(list, sizeof list); speaker_list_user(ulist, sizeof ulist);
    o += snprintf(b + o, cap - o, "\"builtin\":[");
    int first = 1;
    for (char *t = strtok(list, " "); t; t = strtok(NULL, " ")) { o += snprintf(b + o, cap - o, "%s\"%s\"", first ? "" : ",", t); first = 0; }
    o += snprintf(b + o, cap - o, "],\"user\":[");
    first = 1;
    for (char *t = strtok(ulist, " "); t; t = strtok(NULL, " ")) {
        char *c2 = strchr(t, ':'); long ds = 0; if (c2) { *c2 = 0; ds = atol(c2 + 1); }
        o += snprintf(b + o, cap - o, "%s{\"n\":\"%s\",\"s\":%ld.%ld}", first ? "" : ",", t, ds / 10, ds % 10); first = 0;
    }
    { char qe[200]; json_esc(qr_status(), qe, sizeof qe);
      o += snprintf(b + o, cap - o, "],\"free\":%lld,\"max_sec\":%d,\"qr\":{\"running\":%d,\"status\":\"%s\"}}",
                    speaker_user_free_bytes(), SPEAKER_USER_MAX_SAMPLES / 8000, qr_running(), qe); }
    json(fd, 200, b);
    free(b);
}

/* ---------------- OTA relay ---------------- */
static void do_ota(req_t *r)
{
    if (r->clen != OTA_APP_SIZE) { jerr(r->fd, 400, "obraz musí mít přesně 5242880 B (soubor ak_rtsp_firmware_*.squashfs)"); return; }
    const char *tmp = "/tmp/web_ota.bin";
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) { jerr(r->fd, 500, "nelze zapsat /tmp"); return; }
    md5_ctx mc; md5_init(&mc);
    char *buf = malloc(16384); long got = 0; int bad = 0;
    unsigned char magic[4] = { 0 };
    while (buf && got < r->clen) {
        long want = r->clen - got; if (want > 16384) want = 16384;
        ssize_t n = recv(r->fd, buf, want, 0);
        if (n <= 0) { bad = 1; break; }
        if (got < 4) memcpy(magic + got, buf, (size_t)(n < 4 - got ? n : 4 - got));
        md5_update(&mc, buf, n);
        if (write(out, buf, n) != n) { bad = 1; break; }
        got += n;
    }
    close(out);
    if (bad || !buf || memcmp(magic, "hsqs", 4)) { free(buf); unlink(tmp); jerr(r->fd, 400, bad ? "přenos selhal" : "není to squashfs obraz"); return; }
    uint8_t d[16]; md5_final(&mc, d); char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", d[i]);

    ota_enable();
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(OTA_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { free(buf); close(s); unlink(tmp); jerr(r->fd, 500, "OTA služba neběží"); return; }
    char hdr[96]; int hl = snprintf(hdr, sizeof hdr, "LSCFW %d %s\n", OTA_APP_SIZE, hex);
    wall(s, hdr, hl);
    int in = open(tmp, O_RDONLY); ssize_t n;
    while (in >= 0 && (n = read(in, buf, 16384)) > 0) if (wall(s, buf, n)) break;
    if (in >= 0) close(in);
    unlink(tmp);
    /* collect OTA replies until it reports a verdict (flashing takes ~25 s) */
    char log[2048]; size_t lo = 0;
    struct timeval tv = { 90, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (lo < sizeof log - 1) {
        n = recv(s, log + lo, sizeof log - 1 - lo, 0);
        if (n <= 0) break;
        lo += n; log[lo] = 0;
        if (strstr(log, "OTA_FLASH_OK") || strstr(log, "OTA_FAILED")) break;
    }
    log[lo] = 0; close(s); free(buf);
    char e[2200]; json_esc(log, e, sizeof e);
    char *resp = malloc(2400);
    snprintf(resp, 2400, "{\"ok\":%d,\"md5\":\"%s\",\"log\":\"%s\"}", strstr(log, "OTA_FLASH_OK") ? 1 : 0, hex, e);
    json(r->fd, 200, resp); free(resp);
}

/* ---------------- routing ---------------- */
static void handle(req_t *r)
{
    int mode = auth_mode();
    int authed = mode == 2 || sess_ok(r->cookie_sid);
    int setup = mode == 0;
    const char *p = r->path;

    if (!strcmp(r->method, "GET") && (!strcmp(p, "/") || !strcmp(p, "/index.html"))) {
        respond(r->fd, 200, "text/html; charset=utf-8", NULL, (const char *)web_index_html, web_index_html_len); return;
    }
    if (!strcmp(p, "/favicon.svg")) {
        respond(r->fd, 200, "image/svg+xml", "Cache-Control: max-age=86400\r\n", (const char *)web_favicon_svg, web_favicon_svg_len); return;
    }
    if (strncmp(p, "/api/", 5)) { respond(r->fd, 404, "text/plain", NULL, "404", 3); return; }
    const char *api = p + 5;

    if (!strcmp(api, "state") && !authed) {
        json(r->fd, setup ? 200 : 401, setup ? "{\"ok\":0,\"setup\":1}" : "{\"ok\":0,\"login\":1}"); return;
    }
    if (!strcmp(api, "setup") || !strcmp(api, "login")) {
        char *body = read_body(r->fd, r->clen, 2048); if (!body) { jerr(r->fd, 400, "bad body"); return; }
        char pw[100]; form_get(body, "pass", pw, sizeof pw); free(body);
        if (!strcmp(api, "setup")) {
            if (!setup) { jerr2(r->fd, 403, "already", "heslo už je nastavené"); return; }
            if (!pw[0]) { if (pass_set_none()) { jerr(r->fd, 500, "nelze uložit"); return; } jok(r->fd); return; }
            if (strlen(pw) < 6) { jerr2(r->fd, 400, "short", "heslo musí mít aspoň 6 znaků"); return; }
            if (pass_store(pw)) { jerr(r->fd, 500, "nelze uložit"); return; }
        } else {
            if (time(NULL) < g_fail_until) { jerr2(r->fd, 429, "toomany", "příliš mnoho pokusů, počkej chvíli"); return; }
            if (!pass_check(pw)) {
                if (++g_fail_count >= 5) { g_fail_until = time(NULL) + 30; g_fail_count = 0; }
                sleep(1); jerr2(r->fd, 401, "badpass", "špatné heslo"); return;
            }
            g_fail_count = 0;
        }
        char hdr[128]; snprintf(hdr, sizeof hdr, "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Strict\r\n", sess_new());
        respond(r->fd, 200, "application/json", hdr, "{\"ok\":1}", 8); return;
    }
    if (!authed) { jerr(r->fd, 401, "nepřihlášeno"); return; }

    if (!strcmp(api, "state")) { state_json(r->fd); return; }
    if (!strcmp(api, "logout")) { sess_drop(r->cookie_sid); respond(r->fd, 200, "application/json", "Set-Cookie: sid=; Path=/; Max-Age=0\r\n", "{\"ok\":1}", 8); return; }
    if (!strcmp(api, "ota")) { do_ota(r); return; }
    if (!strcmp(api, "frame")) {   /* LEO build17: raw NV12 640x360 sub-channel snapshot */
        if (!g_vi_sub_bufs[0]) { jerr(r->fd, 500, "no frame"); return; }
        size_t n = 640 * 360 * 3 / 2;
        char *copy = malloc(n); if (!copy) { jerr(r->fd, 500, "mem"); return; }
        memcpy(copy, g_vi_sub_bufs[0], n);
        respond(r->fd, 200, "application/octet-stream", NULL, copy, n); free(copy); return;
    }

    char name[48]; form_get(r->query, "name", name, sizeof name);
    if (!strcmp(api, "sound") || !strcmp(api, "playraw")) {
        long max = (long)SPEAKER_USER_MAX_SAMPLES * 2;
        if (r->clen <= 0 || r->clen > max) { jerr2(r->fd, 413, "long", "zvuk je moc dlouhý (max 20 s)"); return; }
        char *body = read_body(r->fd, r->clen, max); if (!body) { jerr(r->fd, 400, "přenos selhal"); return; }
        int rc;
        if (!strcmp(api, "playraw")) { speaker_push((int16_t *)body, (size_t)r->clen / 2); rc = 0; }
        else { rc = speaker_save_user(name, (int16_t *)body, (size_t)r->clen / 2); if (!rc) mqtt_rediscover(); }
        free(body);
        if (rc == -ENOSPC) jerr2(r->fd, 507, "nospace", "v kameře není dost místa");
        else if (rc == -EEXIST) jerr2(r->fd, 400, "builtin", "vestavěný zvuk nelze přepsat");
        else if (rc == -EINVAL) jerr2(r->fd, 400, "badname", "neplatný název (a-z, 0-9, _ -)");
        else if (rc) jerr(r->fd, 500, "uložení selhalo");
        else jok(r->fd);
        return;
    }

    char *body = read_body(r->fd, r->clen > 0 ? r->clen : 0, 4096);
    if (!body) { jerr(r->fd, 400, "bad body"); return; }

    if (!strcmp(api, "set")) {
        char prm[48], val[48]; form_get(body, "param", prm, sizeof prm); form_get(body, "value", val, sizeof val);
        static const char *allowed[] = { "night.mode","ir.led","led.state","image.flip","image.mirror","audio.volume",
            "isp.brightness","isp.contrast","isp.saturation","isp.sharpness","ae.enabled","ae.exp_max", NULL };
        int ok = 0; for (int i = 0; allowed[i]; i++) if (!strcmp(prm, allowed[i])) ok = 1;
        if (!ok) jerr2(r->fd, 400, "param", "nepovolený parametr");
        else if (control_api_set(prm, val, 1)) jerr2(r->fd, 400, "value", "hodnotu nelze nastavit");
        else jok(r->fd);
    } else if (!strcmp(api, "mqtt")) {
        mqtt_cfg_t c; mqtt_get_cfg(&c);
        char t[100];
        form_get(body, "enabled", t, sizeof t); c.enabled = atoi(t) ? 1 : 0;
        form_get(body, "host", c.host, sizeof c.host);
        form_get(body, "port", t, sizeof t); c.port = atoi(t) > 0 ? atoi(t) : 1883;
        form_get(body, "user", c.user, sizeof c.user);
        if (!form_get(body, "pass", t, sizeof t) && t[0]) snprintf(c.pass, sizeof c.pass, "%s", t);
        if (!form_get(body, "clearpass", t, sizeof t) && atoi(t)) c.pass[0] = 0;
        if (!form_get(body, "name", t, sizeof t) && t[0]) snprintf(c.name, sizeof c.name, "%s", t);
        if (strchr(c.host, '\n') || strchr(c.user, '\n') || strchr(c.pass, '\n') || strchr(c.name, '"')) jerr2(r->fd, 400, "badchars", "neplatné znaky");
        else if (mqtt_set_cfg(&c)) jerr(r->fd, 500, "nelze uložit");
        else jok(r->fd);
    } else if (!strcmp(api, "password")) {
        char o[100], n[100]; form_get(body, "old", o, sizeof o); form_get(body, "new", n, sizeof n);
        if (mode == 1 && !pass_check(o)) jerr2(r->fd, 400, "oldbad", "současné heslo nesouhlasí");
        else if (!n[0]) { if (pass_set_none()) jerr(r->fd, 500, "nelze uložit"); else jok(r->fd); }
        else if (strlen(n) < 6) jerr2(r->fd, 400, "short", "heslo musí mít aspoň 6 znaků");
        else if (pass_store(n)) jerr(r->fd, 500, "nelze uložit");
        else jok(r->fd);
    } else if (!strcmp(api, "name")) {
        char n[48]; form_get(body, "name", n, sizeof n);
        if (!n[0] || strchr(n, '"') || strchr(n, '\\') || strchr(n, '\n')) jerr2(r->fd, 400, "badname", "neplatný název");
        else { mqtt_cfg_t c; mqtt_get_cfg(&c); snprintf(c.name, sizeof c.name, "%s", n);
               if (mqtt_set_cfg(&c)) jerr(r->fd, 500, "nelze uložit"); else jok(r->fd); }
    } else if (!strcmp(api, "play")) {
        if (speaker_play(name)) jerr2(r->fd, 404, "notfound", "zvuk nenalezen"); else jok(r->fd);
    } else if (!strcmp(api, "qr")) {
        int rc = qr_wifi_start(120); if (rc < 0) jerr(r->fd, 500, "nelze spustit"); else jok(r->fd);
    } else if (!strcmp(api, "qr_cancel")) {
        qr_cancel(); jok(r->fd);
    } else if (!strcmp(api, "restore_sounds")) {
        speaker_restore_builtins(); mqtt_rediscover(); jok(r->fd);
    } else if (!strcmp(api, "stop")) {
        speaker_stop(); jok(r->fd);
    } else if (!strcmp(api, "delete")) {
        if (speaker_is_builtin(name)) { if (speaker_hide_builtin(name)) jerr(r->fd, 500, "nelze uložit"); else { mqtt_rediscover(); jok(r->fd); } }
        else if (speaker_delete_user(name)) jerr2(r->fd, 404, "notfound", "zvuk nenalezen");
        else { mqtt_rediscover(); jok(r->fd); }
    } else if (!strcmp(api, "restart") || !strcmp(api, "defaults")) {
        if (!strcmp(api, "defaults")) { unlink(CONTROL_CONFIG_FILE); sync(); }
        jok(r->fd); free(body); close(r->fd);
        printf("[web] %s -> reboot\n", api); sync(); sleep(1); reboot(RB_AUTOBOOT);
        return;
    } else {
        jerr(r->fd, 404, "neznámé API");
    }
    free(body);
}

static void parse_and_handle(int fd)
{
    char head[8192];
    if (read_head(fd, head, sizeof head) < 0) return;
    req_t r; memset(&r, 0, sizeof r); r.fd = fd; r.clen = 0;
    char target[400] = "";
    if (sscanf(head, "%7s %399s", r.method, target) != 2) return;
    char *q = strchr(target, '?');
    if (q) { *q = 0; snprintf(r.query, sizeof r.query, "%s", q + 1); }
    snprintf(r.path, sizeof r.path, "%s", target);
    for (char *l = strstr(head, "\r\n"); l && l[2]; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        if (!strncasecmp(h, "Content-Length:", 15)) r.clen = atol(h + 15);
        else if (!strncasecmp(h, "Cookie:", 7)) {
            char *s = strstr(h, "sid=");
            if (s) { s += 4; size_t i = 0; while (s[i] && s[i] != ';' && s[i] != '\r' && i < sizeof r.cookie_sid - 1) { r.cookie_sid[i] = s[i]; i++; } r.cookie_sid[i] = 0; }
        }
    }
    handle(&r);
}

static void *web_conn(void *arg)
{
    int fd = (int)(long)arg;
    struct timeval tv = { 30, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    parse_and_handle(fd);
    shutdown(fd, SHUT_WR);
    close(fd);
    return NULL;
}

static void *web_loop(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(WEB_PORT); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { perror("[web] bind"); close(s); return NULL; }
    printf("[web] http server on port %d\n", WEB_PORT);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) { if (errno != EINTR) sleep(1); continue; }
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 128 * 1024);
        if (pthread_create(&t, &at, web_conn, (void *)(long)fd) != 0) close(fd);
        pthread_attr_destroy(&at);
    }
    return NULL;
}

int web_start(void)
{
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    if (pthread_create(&t, NULL, web_loop, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
