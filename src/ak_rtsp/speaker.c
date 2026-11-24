/* LEO 2026-11-24: speaker output - see speaker.h */
#include <stdio.h>
#include "leo_time.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "speaker.h"

#define SPK_RING_SAMPLES (8000 * 30)   /* 30 s buffer */
#define SPK_AMP_GPIO     69
#define SPK_AMP_OFF_MS   800

static int16_t         g_ring[SPK_RING_SAMPLES];
static size_t          g_head = 0, g_count = 0;          /* read pos, samples queued */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_space = PTHREAD_COND_INITIALIZER;
static volatile int    g_volume = 70;                     /* percent */
static volatile int    g_amp_on = 0;
static long long       g_last_audio_ms = 0;
static volatile unsigned g_generation = 0;               /* bumps on STOP to abort streams */

static long long now_ms(void) { return leo_mono_ms(); }

static void gpio_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY); if (fd < 0) return;
    (void)!write(fd, val, strlen(val)); close(fd);
}

extern int audio_speaker_amp(int on);
static void amp_set(int on)
{
    if (g_amp_on == on) return;
    g_amp_on = on;
    audio_speaker_amp(on);
}

void speaker_set_volume(int v) { if (v < 0) v = 0; if (v > 200) v = 200; g_volume = v; }
int  speaker_get_volume(void)  { return g_volume; }
int  speaker_is_playing(void)  { return g_count > 0; }

/* enqueue samples; block=1 waits for space (stream), block=0 drops overflow */
static size_t speaker_enqueue(const int16_t *s, size_t n, int block, unsigned gen)
{
    size_t done = 0;
    pthread_mutex_lock(&g_lock);
    while (done < n) {
        if (gen != g_generation) break;
        size_t space = SPK_RING_SAMPLES - g_count;
        if (space == 0) {
            if (!block) break;
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
            pthread_cond_timedwait(&g_space, &g_lock, &ts);
            continue;
        }
        size_t tail = (g_head + g_count) % SPK_RING_SAMPLES;
        size_t chunk = n - done;
        if (chunk > space) chunk = space;
        if (chunk > SPK_RING_SAMPLES - tail) chunk = SPK_RING_SAMPLES - tail;
        memcpy(&g_ring[tail], s + done, chunk * sizeof(int16_t));
        g_count += chunk; done += chunk;
    }
    pthread_mutex_unlock(&g_lock);
    return done;
}

void speaker_fill(int16_t *out, size_t n)
{
    size_t got = 0;
    pthread_mutex_lock(&g_lock);
    while (got < n && g_count > 0) {
        size_t chunk = n - got;
        if (chunk > g_count) chunk = g_count;
        if (chunk > SPK_RING_SAMPLES - g_head) chunk = SPK_RING_SAMPLES - g_head;
        memcpy(out + got, &g_ring[g_head], chunk * sizeof(int16_t));
        g_head = (g_head + chunk) % SPK_RING_SAMPLES;
        g_count -= chunk; got += chunk;
    }
    if (got) pthread_cond_broadcast(&g_space);
    pthread_mutex_unlock(&g_lock);

    if (got < n) memset(out + got, 0, (n - got) * sizeof(int16_t));
    if (got) {
        int vol = g_volume;
        for (size_t i = 0; i < got; i++) {
            int v = (int)out[i] * vol / 100;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            out[i] = (int16_t)v;
        }
        g_last_audio_ms = now_ms();
        amp_set(1);
    } else if (g_amp_on && now_ms() - g_last_audio_ms > SPK_AMP_OFF_MS) {
        amp_set(0);
    }
}

void speaker_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_generation++;
    g_head = 0; g_count = 0;
    pthread_cond_broadcast(&g_space);
    pthread_mutex_unlock(&g_lock);
}

static int valid_name(const char *n)
{
    if (!*n || strlen(n) > 40) return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

#include "speaker_user.inc"

/* LEO build17: system voice prompts in SPEAKER_SOUND_DIR/sys (not listed, not deletable) */
int speaker_play_sys(const char *name)
{
    if (!valid_name(name)) return -1;
    char path[112]; snprintf(path, sizeof path, SPEAKER_SOUND_DIR "/sys/%s.adp", name);
    if (access(path, F_OK) == 0) return play_adp_path(path, name);   /* bilingual prompts, ADPCM */
    snprintf(path, sizeof path, SPEAKER_SOUND_DIR "/sys/%s.s16", name);
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    unsigned gen = g_generation; int16_t buf[1024]; ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        if (speaker_enqueue(buf, (size_t)r / 2, 0, gen) < (size_t)r / 2) break;
    close(fd);
    return 0;
}

/* LEO build24: speak the camera's IP address, English then Czech */
const char *net_ip_string(void);
static void spk_gap(int ms)
{
    static const int16_t z[400] = { 0 };
    int n = 8 * ms;
    while (n > 0) { int c = n > 400 ? 400 : n; speaker_enqueue(z, (size_t)c, 0, g_generation); n -= c; }
}
static void say_ip_lang(const char *ip, const char *lang)
{
    char nm[24];
    snprintf(nm, sizeof nm, "ip_%s_intro", lang); speaker_play_sys(nm); spk_gap(180);
    for (const char *c = ip; *c; c++) {
        if (*c >= '0' && *c <= '9') snprintf(nm, sizeof nm, "ip_%s_%c", lang, *c);
        else if (*c == '.') snprintf(nm, sizeof nm, "ip_%s_dot", lang);
        else continue;
        speaker_play_sys(nm); spk_gap(70);
    }
}
void speaker_say_ip(int interrupt)
{
    const char *ip = net_ip_string();
    if (interrupt) speaker_stop();
    if (!ip || !ip[0] || ip[0] == '?' || !strncmp(ip, "0.", 2)) { speaker_play_sys("ip_none"); return; }
    printf("[speaker] saying IP %s\n", ip);
    say_ip_lang(ip, "en"); spk_gap(450); say_ip_lang(ip, "cz");
}

/* LEO build25: boot announcements
 *  - first start of a new firmware version (after SD install / OTA): fw_ready
 *    ("The camera is now running the LSC leotech firmware.") - remembered in /etc/config/leo_fw_seen
 *  - no network 60 s after start: wifi_none (how to connect by QR code), repeated 2x every 5 min */
#include <pthread.h>
#include "qr.h"
const char *build_version_string(void);
#define LEO_FW_SEEN "/etc/config/leo_fw_seen"
static int net_ok(void)
{
    const char *ip = net_ip_string();
    return ip && ip[0] && ip[0] != '?' && strncmp(ip, "0.", 2);
}
static void *boot_announce(void *arg)
{
    (void)arg;
    sleep(10);
    const char *ver = build_version_string();
    char old[160] = { 0 };
    FILE *f = fopen(LEO_FW_SEEN, "r");
    if (f) { if (!fgets(old, sizeof old, f)) old[0] = 0; fclose(f); }
    old[strcspn(old, "\r\n")] = 0;
    if (strcmp(old, ver)) {
        printf("[speaker] first start of %s - announcing\n", ver);
        speaker_play_sys("fw_ready");
        f = fopen(LEO_FW_SEEN ".tmp", "w");
        if (f) { fprintf(f, "%s\n", ver); fclose(f); rename(LEO_FW_SEEN ".tmp", LEO_FW_SEEN); sync(); }
    }
    sleep(50);
    for (int n = 0; n < 3; n++) {
        if (net_ok()) break;
        if (!qr_running() && !speaker_is_playing()) {
            printf("[speaker] no network - Wi-Fi help prompt\n");
            speaker_play_sys("wifi_none");
        }
        for (int s = 0; s < 300 && !net_ok(); s++) sleep(1);
    }
    return NULL;
}
void speaker_boot_announce_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, boot_announce, NULL) == 0) pthread_detach(t);
}

int speaker_play(const char *name)
{
    if (!valid_name(name)) return -1;
    if (speaker_builtin_hidden(name)) return -1;   /* LEO build15 */
    char path[96]; snprintf(path, sizeof path, SPEAKER_SOUND_DIR "/%s.s16", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return play_user(name);   /* LEO build13: user sounds */
    unsigned gen = g_generation;
    int16_t buf[1024]; ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        if (speaker_enqueue(buf, (size_t)r / 2, 0, gen) < (size_t)r / 2) break;  /* full: drop rest */
    close(fd);
    printf("[speaker] play %s\n", name);
    return 0;
}

int speaker_list(char *out, size_t cap)
{
    out[0] = 0;
    DIR *d = opendir(SPEAKER_SOUND_DIR); if (!d) return 0;
    struct dirent *e; int n = 0; size_t len = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".s16")) continue;
        { char nm[64]; if (l - 4 < sizeof nm) { memcpy(nm, e->d_name, l - 4); nm[l - 4] = 0; if (speaker_builtin_hidden(nm)) continue; } }
        if (len + l + 1 >= cap) break;
        if (len) out[len++] = ' ';
        memcpy(out + len, e->d_name, l - 4); len += l - 4; out[len] = 0; n++;
    }
    closedir(d);
    return n;
}

/* ---- network stream: TCP 8093 ---- */
static void handle_stream(int fd)
{
    unsigned gen = g_generation;
    unsigned char hdr[44];
    ssize_t h = recv(fd, hdr, sizeof hdr, MSG_WAITALL);
    if (h <= 0) return;
    int16_t carry[2048];
    if (h >= 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        /* standard 44-byte header assumed (ffmpeg/sox output); check format */
        unsigned ch = hdr[22] | hdr[23] << 8, rate = hdr[24] | hdr[25] << 8 | hdr[26] << 16, bits = hdr[34] | hdr[35] << 8;
        if (ch != 1 || rate != 8000 || bits != 16)
            printf("[speaker] WARN wav %u ch %u Hz %u bit - expected mono 8000 Hz 16 bit\n", ch, rate, bits);
    } else {
        speaker_enqueue((int16_t *)hdr, (size_t)h / 2, 1, gen);   /* raw: header bytes are audio */
    }
    printf("[speaker] stream started\n");
    for (;;) {
        ssize_t r = recv(fd, carry, sizeof carry, 0);
        if (r <= 0) break;
        if (speaker_enqueue(carry, (size_t)r / 2, 1, gen) < (size_t)r / 2) break;  /* STOP */
    }
    printf("[speaker] stream ended\n");
}

static void *stream_loop(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return NULL;
    int opt = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(SPEAKER_STREAM_PORT);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) { perror("[speaker] bind"); close(s); return NULL; }
    printf("[speaker] stream server on port %d\n", SPEAKER_STREAM_PORT);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        struct timeval tv = { 10, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle_stream(fd);
        close(fd);
    }
    return NULL;
}

int speaker_start(void)
{
    g_amp_on = 0;   /* amp = ak_pcm speak-gpios via audio_speaker_amp() */
    pthread_t t;
    if (pthread_create(&t, NULL, stream_loop, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
