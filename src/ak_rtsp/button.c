/* LEO build13: reset button (ak_gpio_keys -> /dev/input/eventX).
 *   hold >= 5 s  (beep while holding)        -> clear web password      (2 beeps)
 *   hold >= 15 s (ding_ding_ding while hold) -> settings + MQTT + web password
 *                                              reset, reboot            (3 beeps)
 *   short press (< 2 s)                     -> say the IP address (EN + CZ)
 *   Wi-Fi configuration is never touched.
 *   hold 2-5 s (1 beep)                     -> Wi-Fi setup by QR code (again = cancel)                          */
#include <stdio.h>
#include "leo_time.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <sys/reboot.h>
#include <stdint.h>
/* LEO build20: the kernel is 32-bit with a 32-bit timeval -> input events are
 * 16 bytes.  musl's 64-bit time_t makes <linux/input.h>'s struct 24 bytes, so
 * read(fd, &ev, sizeof ev) never matched (button "did nothing").  Own layout: */
struct kev { uint32_t sec, usec; uint16_t type, code; int32_t value; };
#define EV_KEY 0x01
#include "web.h"
#include "mqtt.h"
#include "control.h"
#include "speaker.h"
#include "qr.h"

static long long now_ms(void) { return leo_mono_ms(); }

static void beeps(int n)
{
    speaker_stop();
    for (int i = 0; i < n; i++) speaker_play_sys("beep");
}

static void *button_loop(void *arg)
{
    (void)arg;
    int fds[4], nfd = 0;
    for (int i = 0; i < 4; i++) {
        char p[32]; snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) { fds[nfd++] = fd; printf("[button] listening on %s\n", p); }
    }
    if (!nfd) { printf("[button] no input device - reset button inactive\n"); return NULL; }

    long long down_at = 0; int stage = 0;
    for (;;) {
        struct pollfd pf[4];
        for (int i = 0; i < nfd; i++) { pf[i].fd = fds[i]; pf[i].events = POLLIN; }
        poll(pf, nfd, 200);
        for (int i = 0; i < nfd; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            struct kev ev;
            ssize_t rn;
            while ((rn = read(fds[i], &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (ev.type) printf("[button] ev type=%u code=%u value=%d\n", ev.type, ev.code, ev.value);
                if (ev.type != EV_KEY || ev.value == 2) continue;
                if (ev.value == 1) { down_at = now_ms(); stage = 0; printf("[button] key %d down\n", ev.code); }
                else if (down_at) {
                    long long held = now_ms() - down_at; down_at = 0;
                    printf("[button] key %d up after %lld ms\n", ev.code, held);
                    if (held >= 15000) {
                        printf("[button] FACTORY: settings, MQTT, web password reset (Wi-Fi kept)\n");
                        unlink(CONTROL_CONFIG_FILE); unlink(MQTT_CONFIG_FILE); web_reset_password();
                        sync(); speaker_stop(); speaker_play_sys("factory"); sleep(4); reboot(RB_AUTOBOOT);
                    } else if (held >= 5000) {
                        web_reset_password(); speaker_stop(); speaker_play_sys("pass_reset");
                    } else if (held >= 2000) {
                        if (qr_running()) qr_cancel(); else qr_wifi_start(120);
                    } else if (held >= 50 && !qr_running()) {
                        speaker_say_ip(1);        /* short press: say the IP address */
                    }
                }
            }
        }
        if (down_at) {   /* feedback while holding */
            long long held = now_ms() - down_at;
            if (stage == 0 && held >= 2000)  { stage = 1; speaker_stop(); speaker_play_sys("beep"); }
            if (stage == 1 && held >= 5000)  { stage = 2; speaker_stop(); beeps(2); }
            if (stage == 2 && held >= 15000) { stage = 3; speaker_stop(); speaker_play_sys("ding3"); }
        }
    }
    return NULL;
}

int button_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, button_loop, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
