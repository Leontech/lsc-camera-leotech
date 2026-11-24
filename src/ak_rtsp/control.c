#include <stdio.h>
#include "leo_time.h"
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "ae.h"
#include "night.h"
#include "isp.h"
#include "control.h"
#include "web.h"
#include "qr.h"
#include "ircut.h"
#include "ota.h"
#include "speaker.h"
const char *build_version_string(void);  /* version.c (always rebuilt) */
#include <sys/stat.h>

#define CONTROL_PORT 8091

static volatile int    g_control_running;
static pthread_t       g_control_tid;
static pthread_t       g_log_tid;
static int             g_server_fd = -1;

/* Single-client model — this is a debug/tuning tool, not a multi-user
 * service. A new connection replaces any existing one. */
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_client_fd = -1;

static int             g_orig_stdout = -1; /* real stdout fd (serial console) */
static int             g_log_pipe_r  = -1;
/* LEO: keep the last log lines in RAM (LOGS command) - boot log is otherwise lost */
#define LOG_RING_LINES 300
static char            g_log_ring[LOG_RING_LINES][160];
static unsigned        g_log_ring_n = 0;
static pthread_mutex_t g_log_ring_lock = PTHREAD_MUTEX_INITIALIZER;
extern volatile unsigned g_play_writes, g_play_errs; extern volatile int g_play_last_errno, g_play_last_ret;
int audio_playback_running(void); int audio_playback_fd(void);

/* isp.saturation/contrast/brightness/sharpness — the isp_set_*() functions
 * (isp.c) are one-way (GET-modify-SET against the hardware, no getter for
 * "what did we last ask for"), so the logical -50..50 value the user set is
 * tracked here, same ownership pattern as ae_tuning_t/night_tuning_t but
 * without a matching isp_tuning_t struct. 0 = neutral/unchanged from stock
 * default (matches isp_set_* treating the value as a delta off the
 * hardware/cached baseline captured at first call). */
static int g_isp_saturation = 0;
static int g_isp_contrast   = 0;
static int g_isp_brightness = 0;
static int g_isp_sharpness  = 0;

/* -------------------------------------------------------------------------
 * stdout tee — captures every printf in the whole binary (not just ae.c/
 * night.c) without touching a single existing call site. control_start()
 * redirects STDOUT_FILENO into a pipe; this thread reads it, writes the
 * exact same bytes back to the real stdout (serial console unaffected),
 * and additionally forwards complete lines to the connected control client
 * prefixed "LOG ".
 * ------------------------------------------------------------------------- */
static void *log_pump(void *arg)
{
    (void)arg;
    char   raw[512];
    char   line[1024];
    size_t line_len = 0;

    for (;;) {
        ssize_t n = read(g_log_pipe_r, raw, sizeof(raw));
        if (n <= 0) break;  /* pipe write end closed = process exiting */

        write(g_orig_stdout, raw, n);  /* unchanged passthrough to serial console */

        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\n') {
                line[line_len] = '\0';
                pthread_mutex_lock(&g_log_ring_lock);
                snprintf(g_log_ring[g_log_ring_n % LOG_RING_LINES], sizeof(g_log_ring[0]), "%s", line);
                g_log_ring_n++;
                pthread_mutex_unlock(&g_log_ring_lock);
                pthread_mutex_lock(&g_client_lock);
                if (g_client_fd >= 0)
                    dprintf(g_client_fd, "LOG %s\n", line);
                pthread_mutex_unlock(&g_client_lock);
                line_len = 0;
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Command protocol
 * ------------------------------------------------------------------------- */
/* Locked against g_client_lock — log_pump() also writes to this fd from a
 * different thread (async LOG lines); without a shared lock the two could
 * interleave mid-line on the socket. */
/* LEO build13: fd == -2 -> capture into g_cap (internal API for MQTT / web) */
static char            g_cap[256];
static pthread_mutex_t g_api_lock = PTHREAD_MUTEX_INITIALIZER;

static void reply(int fd, const char *fmt, ...)
{
    char buf[256];
    if (fd == -2) {
        va_list ap2; va_start(ap2, fmt);
        vsnprintf(g_cap, sizeof(g_cap), fmt, ap2);
        va_end(ap2);
        return;
    }
    if (fd < 0) return;  /* LEO: config replay at boot has no client */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len < sizeof(buf) - 1) { buf[len] = '\n'; len++; }
    pthread_mutex_lock(&g_client_lock);
    write(fd, buf, len);
    pthread_mutex_unlock(&g_client_lock);
}

static const char *night_mode_name(night_override_t m)
{
    switch (m) {
    case NIGHT_MODE_FORCE_DAY:   return "day";
    case NIGHT_MODE_FORCE_NIGHT: return "night";
    default:                     return "auto";
    }
}

static int night_mode_parse(const char *s, night_override_t *out)
{
    if (!strcmp(s, "auto"))  { *out = NIGHT_MODE_AUTO;        return 0; }
    if (!strcmp(s, "day"))   { *out = NIGHT_MODE_FORCE_DAY;   return 0; }
    if (!strcmp(s, "night")) { *out = NIGHT_MODE_FORCE_NIGHT; return 0; }
    return -1;
}


/* -------------------------------------------------------------------------
 * LEO 2026-11-24: status LEDs (kernel leds class, ak_leds.ko) + IR LED mode
 * ------------------------------------------------------------------------- */
#define LED_COUNT 3
static const char *g_led_sys[LED_COUNT] = { "blue_led", "red_led", "state_led" };
static const char *g_led_key[LED_COUNT] = { "led.blue", "led.red", "led.state" };
static int         g_led_set[LED_COUNT] = { -1, -1, -1 };  /* -1 = never set (boot default) */

static int led_write(const char *led, const char *file, const char *val)
{
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/leds/%s/%s", led, file);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

static int led_read(int i)
{
    char path[96], buf[16] = {0};
    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", g_led_sys[i]);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    return atoi(buf) ? 1 : 0;
}

static int led_index(const char *key)
{
    for (int i = 0; i < LED_COUNT; i++)
        if (!strcmp(key, g_led_key[i])) return i;
    return -1;
}

static int onoff_parse(const char *v)
{
    if (!strcasecmp(v, "on")  || !strcmp(v, "1")) return 1;
    if (!strcasecmp(v, "off") || !strcmp(v, "0")) return 0;
    return -1;
}

static const char *ir_led_name(int m)
{
    return m == IR_LED_ON ? "on" : m == IR_LED_OFF ? "off" : "auto";
}

static int ir_led_parse(const char *v)
{
    if (!strcasecmp(v, "auto")) return IR_LED_AUTO;
    if (!strcasecmp(v, "on")  || !strcmp(v, "1")) return IR_LED_ON;
    if (!strcasecmp(v, "off") || !strcmp(v, "0")) return IR_LED_OFF;
    return -1;
}

/* LEO 2026-11-24: image flip/mirror via ISP inner cmd 0x40044979
 * (= Ak_ISP_Set_Flip_Mirror in stock anyka_ipc, payload int[2] {flip, mirror}) */
#define ISP_INNER_SET_FLIP_MIRROR 0x40044979
static int g_img_flip = 0, g_img_mirror = 0;
static int image_apply_flip_mirror(int flip, int mirror)
{
    int fm[2] = { flip, mirror };
    int r = isp_tunnel(ISP_INNER_SET_FLIP_MIRROR, fm);
    printf("[control] flip=%d mirror=%d -> %d\n", flip, mirror, r);
    if (r == 0) { g_img_flip = flip; g_img_mirror = mirror; }
    return r;
}

static const char *led_state_name(int i)
{
    int v = led_read(i);
    return v < 0 ? "n/a" : v ? "on" : "off";
}

static void do_list(int fd)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    reply(fd, "ae.stable_range=%d",       a.stable_range);
    reply(fd, "ae.hold_range=%d",         a.hold_range);
    reply(fd, "ae.speed=%d",              a.speed);
    reply(fd, "ae.exp_max=%d",            a.exp_max);
    reply(fd, "ae.enabled=%d",            a.enabled);
    reply(fd, "night.mode=%s",            night_mode_name(n.override));
    reply(fd, "night.state=%s",           night_is_night() ? "night" : "day");
    reply(fd, "night.trigger_hw_exp=%u",  n.trigger_hw_exp);
    reply(fd, "night.day_hw_exp=%u",      n.day_hw_exp);
    reply(fd, "night.confirm_samples=%d", n.confirm_samples);
    reply(fd, "night.lock_ms=%d",         n.lock_ms);
    reply(fd, "isp.saturation=%d",        g_isp_saturation);
    reply(fd, "isp.contrast=%d",          g_isp_contrast);
    reply(fd, "isp.brightness=%d",        g_isp_brightness);
    reply(fd, "isp.sharpness=%d",         g_isp_sharpness);
    /* VENC QP is stock-matched ground truth but
     * not live-settable yet — no confirmed ioctl to update rc params on an
     * already-created encoder channel without a DEACTIVATE/CREATE_ENC
     * cycle. Reported for visibility only. */
    reply(fd, "ir.led=%s",               ir_led_name(ircut_get_ir_led_mode()));
    reply(fd, "ir.state=%s (read-only)",  ircut_ir_led_is_on() ? "on" : "off");
    for (int i = 0; i < LED_COUNT; i++)
        reply(fd, "%s=%s",                g_led_key[i], led_state_name(i));
    reply(fd, "image.flip=%s",            g_img_flip ? "on" : "off");
    reply(fd, "image.mirror=%s",          g_img_mirror ? "on" : "off");
    reply(fd, "audio.volume=%d",          speaker_get_volume());
    reply(fd, "config.saved=%s (read-only)", access(CONTROL_CONFIG_FILE, F_OK) == 0 ? "yes" : "no");
    reply(fd, "fw.version=%s (read-only)", build_version_string());
    reply(fd, "ota.status=%s (read-only)", ota_status());
    reply(fd, "venc.minqp=28 (read-only)");
    reply(fd, "venc.maxqp=43 (read-only)");
    reply(fd, ".");
}

static void do_get(int fd, const char *param)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if      (!strcmp(param, "ae.stable_range"))       reply(fd, "ae.stable_range=%d", a.stable_range);
    else if (!strcmp(param, "ae.hold_range"))         reply(fd, "ae.hold_range=%d", a.hold_range);
    else if (!strcmp(param, "ae.speed"))              reply(fd, "ae.speed=%d", a.speed);
    else if (!strcmp(param, "ae.exp_max"))            reply(fd, "ae.exp_max=%d", a.exp_max);
    else if (!strcmp(param, "ae.enabled"))            reply(fd, "ae.enabled=%d", a.enabled);
    else if (!strcmp(param, "night.mode"))            reply(fd, "night.mode=%s", night_mode_name(n.override));
    else if (!strcmp(param, "night.state"))           reply(fd, "night.state=%s", night_is_night() ? "night" : "day");
    else if (!strcmp(param, "night.trigger_hw_exp"))  reply(fd, "night.trigger_hw_exp=%u", n.trigger_hw_exp);
    else if (!strcmp(param, "night.day_hw_exp"))      reply(fd, "night.day_hw_exp=%u", n.day_hw_exp);
    else if (!strcmp(param, "night.confirm_samples")) reply(fd, "night.confirm_samples=%d", n.confirm_samples);
    else if (!strcmp(param, "night.lock_ms"))         reply(fd, "night.lock_ms=%d", n.lock_ms);
    else if (!strcmp(param, "isp.saturation"))        reply(fd, "isp.saturation=%d", g_isp_saturation);
    else if (!strcmp(param, "isp.contrast"))          reply(fd, "isp.contrast=%d", g_isp_contrast);
    else if (!strcmp(param, "isp.brightness"))        reply(fd, "isp.brightness=%d", g_isp_brightness);
    else if (!strcmp(param, "isp.sharpness"))         reply(fd, "isp.sharpness=%d", g_isp_sharpness);
    else if (!strcmp(param, "ir.led"))                reply(fd, "ir.led=%s", ir_led_name(ircut_get_ir_led_mode()));
    else if (!strcmp(param, "ir.state"))              reply(fd, "ir.state=%s", ircut_ir_led_is_on() ? "on" : "off");
    else if (led_index(param) >= 0)                   reply(fd, "%s=%s", param, led_state_name(led_index(param)));
    else if (!strcmp(param, "image.flip"))            reply(fd, "image.flip=%s", g_img_flip ? "on" : "off");
    else if (!strcmp(param, "image.mirror"))          reply(fd, "image.mirror=%s", g_img_mirror ? "on" : "off");
    else if (!strcmp(param, "audio.volume"))          reply(fd, "audio.volume=%d", speaker_get_volume());
    else if (!strcmp(param, "audio.playing"))         reply(fd, "audio.playing=%s", speaker_is_playing() ? "yes" : "no");
    else if (!strcmp(param, "fw.version"))            reply(fd, "fw.version=%s", build_version_string());
    else if (!strcmp(param, "ota.status"))            reply(fd, "ota.status=%s", ota_status());
    else if (!strcmp(param, "config.saved"))          reply(fd, "config.saved=%s", access(CONTROL_CONFIG_FILE, F_OK) == 0 ? "yes" : "no");
    else if (!strcmp(param, "venc.minqp"))            reply(fd, "venc.minqp=28 (read-only)");
    else if (!strcmp(param, "venc.maxqp"))            reply(fd, "venc.maxqp=43 (read-only)");
    else                                              reply(fd, "ERR unknown param %s", param);
}

static void do_set(int fd, const char *param, const char *value)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if (!strcmp(param, "ae.stable_range")) {
        a.stable_range = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.stable_range=%d", a.stable_range);
    } else if (!strcmp(param, "ae.hold_range")) {
        a.hold_range = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.hold_range=%d", a.hold_range);
    } else if (!strcmp(param, "ae.speed")) {
        a.speed = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.speed=%d", a.speed);
    } else if (!strcmp(param, "ae.exp_max")) {
        a.exp_max = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.exp_max=%d", a.exp_max);
    } else if (!strcmp(param, "ae.enabled")) {
        a.enabled = atoi(value) ? 1 : 0;
        ae_set_tuning(&a);
        reply(fd, "OK ae.enabled=%d", a.enabled);
    } else if (!strcmp(param, "night.mode")) {
        night_override_t m;
        if (night_mode_parse(value, &m) != 0) {
            reply(fd, "ERR invalid night.mode value %s (want auto|day|night)", value);
            return;
        }
        n.override = m;
        night_set_tuning(&n);
        reply(fd, "OK night.mode=%s", night_mode_name(m));
    } else if (!strcmp(param, "night.trigger_hw_exp")) {
        n.trigger_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(fd, "OK night.trigger_hw_exp=%u", n.trigger_hw_exp);
    } else if (!strcmp(param, "night.day_hw_exp")) {
        n.day_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(fd, "OK night.day_hw_exp=%u", n.day_hw_exp);
    } else if (!strcmp(param, "night.confirm_samples")) {
        n.confirm_samples = atoi(value);
        night_set_tuning(&n);
        reply(fd, "OK night.confirm_samples=%d", n.confirm_samples);
    } else if (!strcmp(param, "night.lock_ms")) {
        n.lock_ms = atoi(value);
        night_set_tuning(&n);
        reply(fd, "OK night.lock_ms=%d", n.lock_ms);
    } else if (!strcmp(param, "isp.saturation")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.saturation range [-50,50]"); return; }
        if (isp_set_saturation(v) != 0) { reply(fd, "ERR isp_set_saturation failed"); return; }
        g_isp_saturation = v;
        reply(fd, "OK isp.saturation=%d", v);
    } else if (!strcmp(param, "isp.contrast")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.contrast range [-50,50]"); return; }
        if (isp_set_contrast(v) != 0) { reply(fd, "ERR isp_set_contrast failed"); return; }
        g_isp_contrast = v;
        reply(fd, "OK isp.contrast=%d", v);
    } else if (!strcmp(param, "isp.brightness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.brightness range [-50,50]"); return; }
        if (isp_set_brightness(v) != 0) { reply(fd, "ERR isp_set_brightness failed"); return; }
        g_isp_brightness = v;
        reply(fd, "OK isp.brightness=%d", v);
    } else if (!strcmp(param, "isp.sharpness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.sharpness range [-50,50]"); return; }
        if (isp_set_sharpness(v) != 0) { reply(fd, "ERR isp_set_sharpness failed"); return; }
        g_isp_sharpness = v;
        reply(fd, "OK isp.sharpness=%d", v);
    } else if (!strcmp(param, "image.flip") || !strcmp(param, "image.mirror")) {
        int on = onoff_parse(value);
        if (on < 0) { reply(fd, "ERR invalid %s value %s (want on|off)", param, value); return; }
        int f = g_img_flip, m = g_img_mirror;
        if (!strcmp(param, "image.flip")) f = on; else m = on;
        if (image_apply_flip_mirror(f, m) != 0) { reply(fd, "ERR isp flip/mirror ioctl failed"); return; }
        reply(fd, "OK %s=%s", param, on ? "on" : "off");
    } else if (!strcmp(param, "audio.volume")) {
        int v = atoi(value);
        if (v < 0 || v > 200) { reply(fd, "ERR audio.volume range [0,200]"); return; }
        speaker_set_volume(v);
        reply(fd, "OK audio.volume=%d", v);
    } else if (!strcmp(param, "ir.led")) {
        int m = ir_led_parse(value);
        if (m < 0) { reply(fd, "ERR invalid ir.led value %s (want auto|on|off)", value); return; }
        ircut_set_ir_led_mode(m);
        reply(fd, "OK ir.led=%s", ir_led_name(m));
    } else if (led_index(param) >= 0) {
        int i = led_index(param);
        int on = onoff_parse(value);
        if (on < 0) { reply(fd, "ERR invalid %s value %s (want on|off)", param, value); return; }
        led_write(g_led_sys[i], "trigger", "none");   /* stop any blink timer */
        if (led_write(g_led_sys[i], "brightness", on ? "1" : "0") != 0) {
            reply(fd, "ERR cannot write /sys/class/leds/%s", g_led_sys[i]); return;
        }
        g_led_set[i] = on;
        reply(fd, "OK %s=%s", param, on ? "on" : "off");
    } else if (!strcmp(param, "venc.minqp") || !strcmp(param, "venc.maxqp")) {
        reply(fd, "ERR %s is read-only in this version "
                  "(requires an encoder restart, not yet implemented)", param);
    } else {
        reply(fd, "ERR unknown param %s", param);
    }
}


/* -------------------------------------------------------------------------
 * LEO 2026-11-24: persistence (SAVE / DEFAULTS / boot replay)
 * ------------------------------------------------------------------------- */
static int do_save(int fd)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);
    const char *tmp = CONTROL_CONFIG_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { reply(fd, "ERR cannot write %s: %s", tmp, strerror(errno)); return -1; }

    fprintf(f, "# ak_rtsp persistent settings - written by SAVE on port 8091\n");
    fprintf(f, "ae.stable_range %d\n", a.stable_range);
    fprintf(f, "ae.hold_range %d\n",   a.hold_range);
    fprintf(f, "ae.speed %d\n",        a.speed);
    fprintf(f, "ae.exp_max %d\n",      a.exp_max);
    fprintf(f, "ae.enabled %d\n",      a.enabled);
    fprintf(f, "night.trigger_hw_exp %u\n",  n.trigger_hw_exp);
    fprintf(f, "night.day_hw_exp %u\n",      n.day_hw_exp);
    fprintf(f, "night.confirm_samples %d\n", n.confirm_samples);
    fprintf(f, "night.lock_ms %d\n",         n.lock_ms);
    fprintf(f, "night.mode %s\n",            night_mode_name(n.override));
    fprintf(f, "isp.saturation %d\n", g_isp_saturation);
    fprintf(f, "isp.contrast %d\n",   g_isp_contrast);
    fprintf(f, "isp.brightness %d\n", g_isp_brightness);
    fprintf(f, "isp.sharpness %d\n",  g_isp_sharpness);
    fprintf(f, "ir.led %s\n", ir_led_name(ircut_get_ir_led_mode()));
    fprintf(f, "image.flip %s\n",   g_img_flip ? "on" : "off");
    fprintf(f, "image.mirror %s\n", g_img_mirror ? "on" : "off");
    fprintf(f, "audio.volume %d\n", speaker_get_volume());
    for (int i = 0; i < LED_COUNT; i++) {
        int v = g_led_set[i] >= 0 ? g_led_set[i] : led_read(i);
        if (v >= 0) fprintf(f, "%s %s\n", g_led_key[i], v ? "on" : "off");
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, CONTROL_CONFIG_FILE) != 0) {
        reply(fd, "ERR rename %s: %s", CONTROL_CONFIG_FILE, strerror(errno));
        unlink(tmp);
        return -1;
    }
    sync();
    printf("[control] settings saved to %s\n", CONTROL_CONFIG_FILE);
    reply(fd, "OK saved %s", CONTROL_CONFIG_FILE);
    return 0;
}

int control_load_config(void)
{
    FILE *f = fopen(CONTROL_CONFIG_FILE, "r");
    if (!f) { printf("[control] no saved settings (%s)\n", CONTROL_CONFIG_FILE); return 0; }
    char line[160];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *param = strtok(p, " \t\r\n");
        char *value = strtok(NULL, " \t\r\n");
        if (!param || !value) continue;
        do_set(-1, param, value);
        count++;
    }
    fclose(f);
    printf("[control] applied %d saved settings from %s\n", count, CONTROL_CONFIG_FILE);
    return count;
}

/* Blocking byte-at-a-time line reader — traffic on this protocol is tiny
 * (a human moving sliders), so simplicity wins over buffered-recv efficiency. */
static int read_line(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;  /* EOF or error */
        if (c == '\r') continue;
        if (c == '\n') { buf[i] = '\0'; return (int)i; }
        if (i < cap - 1) buf[i++] = c;
    }
}

static void handle_control_client(int fd)
{
    char line[256];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n < 0) return;
        if (n == 0) continue;

        char *cmd = strtok(line, " ");
        if (!cmd) continue;

        if (!strcasecmp(cmd, "LIST")) {
            do_list(fd);
        } else if (!strcasecmp(cmd, "GET")) {
            char *param = strtok(NULL, " ");
            if (!param) { reply(fd, "ERR GET requires a param name"); continue; }
            do_get(fd, param);
        } else if (!strcasecmp(cmd, "SET")) {
            char *param = strtok(NULL, " ");
            char *value = strtok(NULL, " ");
            if (!param || !value) { reply(fd, "ERR SET requires param and value"); continue; }
            do_set(fd, param, value);
        } else if (!strcasecmp(cmd, "LOGS")) {
            char *nstr = strtok(NULL, " ");
            unsigned want = nstr ? (unsigned)atoi(nstr) : 80;
            pthread_mutex_lock(&g_log_ring_lock);
            unsigned total = g_log_ring_n, have = total < LOG_RING_LINES ? total : LOG_RING_LINES;
            if (want > have) want = have;
            char copy[160];
            for (unsigned i = total - want; i < total; i++) {
                snprintf(copy, sizeof copy, "%s", g_log_ring[i % LOG_RING_LINES]);
                pthread_mutex_unlock(&g_log_ring_lock);
                reply(fd, "L %s", copy);
                pthread_mutex_lock(&g_log_ring_lock);
            }
            pthread_mutex_unlock(&g_log_ring_lock);
            reply(fd, ".");
        } else if (!strcasecmp(cmd, "AUDIODIAG")) {
            reply(fd, "audio.playback_running=%d fd=%d writes=%u errs=%u last_ret=%d last_errno=%d playing=%d",
                  audio_playback_running(), audio_playback_fd(), g_play_writes, g_play_errs, g_play_last_ret, g_play_last_errno, speaker_is_playing());
        } else if (!strcasecmp(cmd, "PLAY")) {
            char *name = strtok(NULL, " ");
            if (!name) { reply(fd, "ERR PLAY requires a sound name (see SOUNDS)"); continue; }
            if (speaker_play(name) == 0) reply(fd, "OK playing %s", name);
            else reply(fd, "ERR unknown sound %s", name);
        } else if (!strcasecmp(cmd, "STOP")) {
            speaker_stop();
            reply(fd, "OK stopped");
        } else if (!strcasecmp(cmd, "CLOCK")) {      /* LEO build21 diag */
            struct timespec a = {0}, b = {0}; int ra = clock_gettime(CLOCK_MONOTONIC, &a), rb = clock_gettime(CLOCK_REALTIME, &b);
            struct timeval tv = {0}; gettimeofday(&tv, NULL);
            reply(fd, "leo_mono_ms=%lld mono rc=%d %lld.%09ld errno=%d real rc=%d %lld tod=%lld time=%lld sizeof(time_t)=%d", leo_mono_ms(), ra, (long long)a.tv_sec, a.tv_nsec, errno, rb, (long long)b.tv_sec, (long long)tv.tv_sec, (long long)time(NULL), (int)sizeof(time_t));
        } else if (!strcasecmp(cmd, "QRWIFI")) {    /* LEO build17: QRWIFI [CANCEL|STATUS] */
            char *a = strtok(NULL, " ");
            if (a && !strcasecmp(a, "CANCEL")) { qr_cancel(); reply(fd, "OK cancel"); }
            else if (a && !strcasecmp(a, "STATUS")) reply(fd, "qr.status=%s running=%d", qr_status(), qr_running());
            else { int rc = qr_wifi_start(120); reply(fd, rc == 0 ? "OK scanning 120 s" : rc == 1 ? "OK already scanning" : "ERR start"); }
        } else if (!strcasecmp(cmd, "WEBPASS")) {   /* LEO build13: WEBPASS RESET */
            char *arg1 = strtok(NULL, " "); if (arg1 && !strcasecmp(arg1, "RESET")) { web_reset_password(); reply(fd, "OK web password cleared"); }
            else reply(fd, "ERR usage: WEBPASS RESET");
        } else if (!strcasecmp(cmd, "SOUNDS")) {
            char list[512]; speaker_list(list, sizeof list);
            reply(fd, "sounds=%s", list);
        } else if (!strcasecmp(cmd, "OTA")) {
            char *sub = strtok(NULL, " ");
            if (sub && !strcasecmp(sub, "ENABLE")) {
                int sec = ota_enable();
                reply(fd, "OK ota enabled for %d s - send image to port %d", sec, OTA_PORT);
            } else {
                reply(fd, "ota.status=%s", ota_status());
                reply(fd, "ota.window=%d", ota_window_left());
            }
        } else if (!strcasecmp(cmd, "SAVE")) {
            do_save(fd);
        } else if (!strcasecmp(cmd, "DEFAULTS")) {
            if (unlink(CONTROL_CONFIG_FILE) == 0 || errno == ENOENT) { sync(); reply(fd, "OK defaults - saved settings removed, effective after restart"); }
            else reply(fd, "ERR unlink %s: %s", CONTROL_CONFIG_FILE, strerror(errno));
        } else {
            reply(fd, "ERR unknown command %s", cmd);
        }
    }
}

static void *control_accept_loop(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[control] socket"); return NULL; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(CONTROL_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[control] bind"); close(server_fd); return NULL;
    }
    if (listen(server_fd, 1) < 0) {
        perror("[control] listen"); close(server_fd); return NULL;
    }

    g_server_fd = server_fd;
    printf("[control] tuning server listening on port %d\n", CONTROL_PORT);

    while (g_control_running) {
        struct sockaddr_in client_addr;
        socklen_t alen = sizeof(client_addr);
        int fd = accept(server_fd, (struct sockaddr *)&client_addr, &alen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  /* server_fd closed by control_stop() */
        }

        pthread_mutex_lock(&g_client_lock);
        if (g_client_fd >= 0) close(g_client_fd);  /* single-client: replace previous */
        g_client_fd = fd;
        pthread_mutex_unlock(&g_client_lock);

        printf("[control] tuning client connected\n");
        handle_control_client(fd);

        pthread_mutex_lock(&g_client_lock);
        if (g_client_fd == fd) { close(fd); g_client_fd = -1; }
        pthread_mutex_unlock(&g_client_lock);
        printf("[control] tuning client disconnected\n");
    }

    return NULL;
}

int control_start(void)
{
    /* Tee stdout BEFORE any other init runs, so the full boot log (ISP,
     * VI, VENC) reaches the tuning tab's debug view, not just ae/night's
     * own logging. */
    g_orig_stdout = dup(STDOUT_FILENO);
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("[control] pipe"); return -1; }
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("[control] dup2"); return -1; }
    close(pipefd[1]);
    g_log_pipe_r = pipefd[0];
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffered so printf flushes promptly through the pipe */

    if (pthread_create(&g_log_tid, NULL, log_pump, NULL) != 0) {
        perror("[control] log_pump pthread_create");
        return -1;
    }

    g_control_running = 1;
    if (pthread_create(&g_control_tid, NULL, control_accept_loop, NULL) != 0) {
        perror("[control] pthread_create");
        g_control_running = 0;
        return -1;
    }
    return 0;
}

void control_stop(void)
{
    g_control_running = 0;
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }
    pthread_mutex_lock(&g_client_lock);
    if (g_client_fd >= 0) {
        /* shutdown() before close() — the accept thread may be blocked in
         * recv() inside handle_control_client(); shutdown() forces that
         * call to return cleanly instead of racing a bare close(). */
        shutdown(g_client_fd, SHUT_RDWR);
        close(g_client_fd);
        g_client_fd = -1;
    }
    pthread_mutex_unlock(&g_client_lock);
    pthread_join(g_control_tid, NULL);
    /* log_pump isn't joined — it blocks on read() of the tee'd stdout pipe
     * and only returns when that pipe closes at process exit, which is
     * always imminent by the time control_stop() runs. Acceptable for a
     * debug tool's shutdown path. */
}


/* -------------------------------------------------------------------------
 * LEO build13: internal API (MQTT, web)
 * ------------------------------------------------------------------------- */
int control_api_set(const char *param, const char *value, int save)
{
    pthread_mutex_lock(&g_api_lock);
    g_cap[0] = 0;
    do_set(-2, param, value);
    int ok = strncmp(g_cap, "OK", 2) == 0;
    if (!ok) printf("[control] api set %s=%s failed: %s\n", param, value, g_cap);
    if (ok && save) do_save(-1);
    pthread_mutex_unlock(&g_api_lock);
    return ok ? 0 : -1;
}

int control_api_get(const char *param, char *out, size_t cap)
{
    pthread_mutex_lock(&g_api_lock);
    g_cap[0] = 0;
    do_get(-2, param);
    int ok = strncmp(g_cap, "ERR", 3) != 0;
    const char *eq = strchr(g_cap, '=');
    snprintf(out, cap, "%s", (ok && eq) ? eq + 1 : "");
    char *sp = strstr(out, " (read-only)"); if (sp) *sp = 0;
    pthread_mutex_unlock(&g_api_lock);
    return ok ? 0 : -1;
}
