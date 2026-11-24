/* leo_say - play voice prompts on the LSC Smart Connect camera (AK3918AV130)
 * from the SD card, both on stock firmware and on ak_rtsp custom firmware.
 *
 *   leo_say file1.adp [file2.s16 ...]     (8 kHz mono; .adp = IMA ADPCM "LADP", .s16 = raw s16le)
 *
 * 1) If the ak_rtsp speaker service listens on 127.0.0.1:8093, the audio is
 *    sent there (the custom firmware owns the audio device).
 * 2) Otherwise /dev/pcmC0D0p is driven directly (ak_pcm.ko ioctls found by
 *    disassembly: commit params, source=DAC, gain, start DMA, speaker amp).
 * MIT licence, (c) 2026 leotech.cz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RATE 8000
#define PERIOD_BYTES 512

static const int IT[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };
static const int ST[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767 };

static int dec1(int *pred, int *idx, int code)
{
    int step = ST[*idx], diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    *pred += (code & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767; else if (*pred < -32768) *pred = -32768;
    *idx += IT[code & 15]; if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
    return *pred;
}

static int16_t *g_pcm = NULL; static size_t g_n = 0, g_cap = 0;
static void push(int16_t s)
{
    if (g_n == g_cap) { g_cap = g_cap ? g_cap * 2 : 65536; g_pcm = realloc(g_pcm, g_cap * 2); if (!g_pcm) exit(2); }
    g_pcm[g_n++] = s;
}

static int load(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "leo_say: %s: %s\n", path, strerror(errno)); return -1; }
    unsigned char h[8];
    size_t r = fread(h, 1, 8, f);
    if (r == 8 && !memcmp(h, "LADP", 4)) {
        uint32_t n = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24, done = 0;
        int pred = 0, idx = 0, c;
        while (done < n && (c = fgetc(f)) != EOF) {
            push((int16_t)dec1(&pred, &idx, c & 15)); done++;
            if (done < n) { push((int16_t)dec1(&pred, &idx, (c >> 4) & 15)); done++; }
        }
    } else {
        int16_t s[2]; fseek(f, 0, SEEK_SET);
        while (fread(s, 2, 1, f) == 1) push(s[0]);
    }
    fclose(f);
    for (int i = 0; i < RATE / 4; i++) push(0);   /* 250 ms gap between files */
    return 0;
}

static int via_service(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(8093); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    const char *p = (const char *)g_pcm; size_t left = g_n * 2;
    while (left) { ssize_t w = send(s, p, left, MSG_NOSIGNAL); if (w <= 0) break; p += w; left -= w; }
    close(s);
    usleep((useconds_t)((g_n * 1000000ULL) / RATE));   /* let it finish before the caller continues */
    return 0;
}

static int via_device(void)
{
    int fd = open("/dev/pcmC0D0p", O_WRONLY);
    if (fd < 0) { fprintf(stderr, "leo_say: /dev/pcmC0D0p: %s\n", strerror(errno)); return -1; }
    struct { uint32_t format, rate, channels, bits, period_size, period_count, reserved; } pp = { 0, RATE, 1, 16, PERIOD_BYTES, 16, 0 };
    int32_t v;
    ioctl(fd, 0x401c50e0, &pp);                 /* commit params (ready bit)      */
    v = 4; ioctl(fd, 0x40045030, &v);           /* source = DAC                   */
    v = 4; ioctl(fd, 0x40045010, &v);           /* gain 0..7                      */
    ioctl(fd, 0x000050e1, 0);                   /* start DMA                      */
    v = 1; ioctl(fd, 0x40045081, &v);           /* speaker amp on                 */
    for (int i = 0; i < RATE / 10; i++) push(0);   /* tail so the amp does not cut the end */
    const char *p = (const char *)g_pcm; size_t left = g_n * 2;
    while (left) {
        size_t chunk = left > PERIOD_BYTES ? PERIOD_BYTES : left;
        char buf[PERIOD_BYTES] = { 0 }; memcpy(buf, p, chunk);
        if (write(fd, buf, PERIOD_BYTES) < 0) { if (errno == EINTR) continue; fprintf(stderr, "leo_say: write: %s\n", strerror(errno)); break; }
        p += chunk; left -= chunk;
    }
    usleep(300000);
    v = 0; ioctl(fd, 0x40045081, &v);           /* speaker amp off                */
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: leo_say file.adp|file.s16 ...\n"); return 1; }
    for (int i = 1; i < argc; i++) load(argv[i]);
    if (!g_n) return 1;
    if (via_service() == 0) return 0;
    return via_device() == 0 ? 0 : 1;
}
