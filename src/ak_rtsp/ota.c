/* LEO 2026-11-24: network firmware update of the APP partition. See ota.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "md5.h"
#include "ota.h"

#define OTA_IMG      "/tmp/ota_app.img"
#define OTA_PREV     "/tmp/ota_prev.img"
#define OTA_TOOL     "/tmp/ota_flash_tool"
#define OTA_SCRIPT   "/tmp/ota_flash.sh"
#define OTA_LOG      "/tmp/ota_flash.log"
#define OTA_RC       "/tmp/ota_flash.rc"
#define APP_MTD      "/dev/mtd7"
#define APP_MTDBLOCK "/dev/mtdblock7"
#define FLASH_TOOL_SRC "/usr/bin/flash_tool"

static volatile time_t g_enabled_until = 0;
static volatile int    g_busy = 0;
static char            g_status[128] = "idle";
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_status(const char *s)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_status, sizeof(g_status), "%s", s);
    pthread_mutex_unlock(&g_lock);
    printf("[ota] %s\n", s);
}
const char *ota_status(void) { return g_status; }

int ota_window_left(void)
{
    time_t now = time(NULL);
    return g_enabled_until > now ? (int)(g_enabled_until - now) : 0;
}

int ota_enable(void)
{
    g_enabled_until = time(NULL) + OTA_WINDOW_SEC;
    set_status("enabled - waiting for image on port 8092");
    return OTA_WINDOW_SEC;
}

static void sreply(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf) - 2, fmt, ap); va_end(ap);
    size_t n = strlen(buf); buf[n++] = '\n';
    (void)!write(fd, buf, n);
}

static int read_header(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    while (i < cap - 1) {
        char c; ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = 0;
    return (int)i;
}

static long mem_available_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128]; long v = -1, mfree = -1, cached = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %ld kB", &v) == 1) break;
        sscanf(line, "MemFree: %ld kB", &mfree);
        sscanf(line, "Cached: %ld kB", &cached);
    }
    fclose(f);
    if (v < 0 && mfree >= 0) v = mfree + (cached > 0 ? cached : 0);
    return v;
}

static int copy_file(const char *src, const char *dst, mode_t mode, long expect)
{
    int in = open(src, O_RDONLY); if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode); if (out < 0) { close(in); return -1; }
    char buf[16384]; long total = 0; ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) { close(in); close(out); return -1; }
        total += n;
        if (expect > 0 && total >= expect) break;
    }
    close(in); fsync(out); close(out);
    return (expect > 0 && total != expect) ? -1 : 0;
}

static void md5_hex_file(const char *path, char out[33])
{
    out[0] = 0;
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    md5_ctx c; md5_init(&c);
    char buf[16384]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) md5_update(&c, buf, n);
    close(fd);
    uint8_t d[16]; md5_final(&c, d);
    for (int i = 0; i < 16; i++) sprintf(out + i*2, "%02x", d[i]);
}

static void handle_ota_client(int fd)
{
    char hdr[160], magic[16], want_md5[64];
    long size = 0;
    if (ota_window_left() <= 0) { sreply(fd, "OTA_FAILED not enabled (send OTA ENABLE on port 8091 first)"); return; }
    if (g_busy) { sreply(fd, "OTA_FAILED busy"); return; }
    if (read_header(fd, hdr, sizeof(hdr)) <= 0 ||
        sscanf(hdr, "%15s %ld %63s", magic, &size, want_md5) != 3 || strcmp(magic, "LSCFW") != 0) {
        sreply(fd, "OTA_FAILED bad header (want: LSCFW <size> <md5>)"); return;
    }
    if (size != OTA_APP_SIZE) { sreply(fd, "OTA_FAILED size %ld != %d", size, OTA_APP_SIZE); return; }
    for (char *p = want_md5; *p; p++) if (*p >= 'A' && *p <= 'F') *p += 32;
    long avail = mem_available_kb();
    sreply(fd, "OTA_INFO mem_available_kb=%ld", avail);
    if (avail >= 0 && avail < 3 * (OTA_APP_SIZE / 1024)) { sreply(fd, "OTA_FAILED not enough RAM (%ld kB)", avail); return; }

    g_busy = 1;
    g_enabled_until = 0;  /* one shot */
    set_status("receiving");
    int out = open(OTA_IMG, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) { sreply(fd, "OTA_FAILED open %s: %s", OTA_IMG, strerror(errno)); goto fail; }
    md5_ctx c; md5_init(&c);
    char buf[16384]; long got = 0; unsigned char first[4] = {0};
    while (got < size) {
        ssize_t n = recv(fd, buf, (size - got) < (long)sizeof(buf) ? (size_t)(size - got) : sizeof(buf), 0);
        if (n <= 0) { close(out); sreply(fd, "OTA_FAILED connection lost at %ld", got); goto fail; }
        if (got == 0 && n >= 4) memcpy(first, buf, 4);
        md5_update(&c, buf, n);
        if (write(out, buf, n) != n) { close(out); sreply(fd, "OTA_FAILED write /tmp: %s", strerror(errno)); goto fail; }
        got += n;
    }
    fsync(out); close(out);
    uint8_t d[16]; md5_final(&c, d); char got_md5[33];
    for (int i = 0; i < 16; i++) sprintf(got_md5 + i*2, "%02x", d[i]);
    sreply(fd, "OTA_INFO received %ld bytes md5=%s", got, got_md5);
    if (strcmp(got_md5, want_md5) != 0) { sreply(fd, "OTA_FAILED md5 mismatch"); goto fail; }
    if (memcmp(first, "hsqs", 4) != 0) { sreply(fd, "OTA_FAILED not a squashfs image"); goto fail; }

    char cur_md5[33];
    md5_hex_file(APP_MTDBLOCK, cur_md5);
    sreply(fd, "OTA_INFO current APP md5=%s", cur_md5);
    if (!strcmp(cur_md5, got_md5)) { sreply(fd, "OTA_FLASH_OK already running this image"); unlink(OTA_IMG); set_status("idle"); g_busy = 0; return; }

    set_status("backing up current APP");
    if (copy_file(APP_MTDBLOCK, OTA_PREV, 0600, OTA_APP_SIZE) != 0) { sreply(fd, "OTA_FAILED backup of current APP"); goto fail; }
    if (copy_file(FLASH_TOOL_SRC, OTA_TOOL, 0755, 0) != 0) { sreply(fd, "OTA_FAILED copy flash_tool (%s missing?)", FLASH_TOOL_SRC); goto fail; }

    FILE *s = fopen(OTA_SCRIPT, "w");
    if (!s) { sreply(fd, "OTA_FAILED write script"); goto fail; }
    fprintf(s,
        "#!/bin/sh\n"
        "# generated by ak_rtsp OTA - runs from RAM, independent of the APP partition\n"
        "rm -f " OTA_RC "\n"
        "echo \"=== OTA flash $(date) ===\" > " OTA_LOG "\n"
        OTA_TOOL " " OTA_IMG " " APP_MTD " >> " OTA_LOG " 2>&1\n"
        "RC=$?\n"
        "if [ \"$RC\" != 0 ]; then\n"
        "  echo \"flash failed rc=$RC, restoring previous APP\" >> " OTA_LOG "\n"
        "  " OTA_TOOL " " OTA_PREV " " APP_MTD " >> " OTA_LOG " 2>&1\n"
        "  echo \"restore rc=$?\" >> " OTA_LOG "\n"
        "fi\n"
        "echo $RC > " OTA_RC "\n"
        "sync\n"
        "if [ \"$RC\" = 0 ]; then sleep 3; reboot; fi\n");
    fclose(s);
    chmod(OTA_SCRIPT, 0755);

    set_status("flashing");
    sreply(fd, "OTA_INFO image verified, flashing APP (do not power off)...");
    /* vfork: no copy of the large (ION/VENC-mapped) address space; child only
     * does setsid + exec. setsid detaches it so it survives ak_rtsp dying. */
    pid_t pid = vfork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", OTA_SCRIPT, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { sreply(fd, "OTA_FAILED fork"); goto fail; }

    /* wait for the detached flasher; this process may itself die while its own
     * binary is being overwritten - the flasher does not depend on it. */
    for (int t = 0; t < 600; t++) {
        sleep(1);
        FILE *r = fopen(OTA_RC, "r");
        if (r) {
            int rc = -1; if (fscanf(r, "%d", &rc) != 1) rc = -1; fclose(r);
            if (rc == 0) { set_status("ok - rebooting"); sreply(fd, "OTA_FLASH_OK rebooting"); }
            else { char m[160]; snprintf(m, sizeof m, "failed: flash rc=%d, previous APP restored (see " OTA_LOG ")", rc); set_status(m); sreply(fd, "OTA_FAILED flash rc=%d previous APP restore attempted", rc); }
            g_busy = 0;
            return;
        }
        if (t % 10 == 0) sreply(fd, "OTA_INFO flashing... %d s", t);
    }
    sreply(fd, "OTA_FAILED timeout waiting for flasher (check " OTA_LOG ")");
    g_busy = 0;
    return;
fail:
    unlink(OTA_IMG);
    set_status("failed");
    g_busy = 0;
}

static void *ota_loop(void *arg)
{
    (void)arg;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("[ota] socket"); return NULL; }
    int opt = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(OTA_PORT);
    if (bind(sfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(sfd, 1) < 0) { perror("[ota] bind/listen"); close(sfd); return NULL; }
    printf("[ota] update server listening on port %d (enable via OTA ENABLE on 8091)\n", OTA_PORT);
    for (;;) {
        int fd = accept(sfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; sleep(1); continue; }
        struct timeval tv = { 30, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle_ota_client(fd);
        close(fd);
    }
    return NULL;
}

int ota_start(void)
{
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    if (pthread_create(&t, NULL, ota_loop, NULL) != 0) { perror("[ota] pthread_create"); return -1; }
    pthread_detach(t);
    return 0;
}
