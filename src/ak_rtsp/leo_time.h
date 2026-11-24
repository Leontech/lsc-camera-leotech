#pragma once
/* LEO build21: monotonic milliseconds.
 * On this camera (old 32-bit kernel, musl time64) clock_gettime(CLOCK_MONOTONIC)
 * returns rc=0 with 0.000000000 -> every timeout based on it never expired.
 * Use the legacy 32-bit clock_gettime syscall directly, fall back to REALTIME. */
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
static inline long long leo_mono_ms(void)
{
    struct { int32_t sec, nsec; } t32 = { 0, 0 };
#ifdef __NR_clock_gettime
    if (syscall(__NR_clock_gettime, CLOCK_MONOTONIC, &t32) == 0 && (t32.sec || t32.nsec))
        return (long long)t32.sec * 1000 + t32.nsec / 1000000;
#else
    if (syscall(263, CLOCK_MONOTONIC, &t32) == 0 && (t32.sec || t32.nsec))
        return (long long)t32.sec * 1000 + t32.nsec / 1000000;
#endif
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
