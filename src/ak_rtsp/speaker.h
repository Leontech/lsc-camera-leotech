#pragma once
/* LEO 2026-11-24: speaker output (8 kHz, mono, s16le) mixed into the existing
 * playback stream that audio.c keeps running (it used to write only silence).
 *  - built-in sounds:  /usr/share/leo_sounds/<name>.s16   (PLAY <name> on 8091)
 *  - network stream:   TCP 8093, raw s16le 8 kHz mono or a WAV file with that format
 *  - volume 0..200 %   (SET audio.volume N)
 * Speaker amplifier = GPIO 69 (speak_en_gpio, active high), switched on while playing. */
#include <stdint.h>
#include <stddef.h>
#define SPEAKER_STREAM_PORT 8093
#define SPEAKER_SOUND_DIR   "/usr/share/leo_sounds"

int  speaker_start(void);                       /* init GPIO + stream server */
void speaker_fill(int16_t *out, size_t n);      /* called by audio playback loop */
int  speaker_play(const char *name);            /* 0 ok, -1 not found */
void speaker_stop(void);                        /* flush queue */
void speaker_set_volume(int v);
int  speaker_get_volume(void);
int  speaker_is_playing(void);
int  speaker_list(char *out, size_t cap);       /* space separated sound names */

/* LEO build13: user sounds (CONFIG jffs2, IMA ADPCM), web upload */
#define SPEAKER_USER_DIR          "/etc/config/sounds"
#define SPEAKER_USER_MAX_SAMPLES  (8000 * 20)
int       speaker_is_builtin(const char *name);
int       speaker_push(const int16_t *s, size_t n);          /* play now (stops current) */
int       speaker_save_user(const char *name, const int16_t *s, size_t n); /* 0 / -errno */
int       speaker_delete_user(const char *name);
long long speaker_user_free_bytes(void);
int       speaker_list_user(char *out, size_t cap);          /* "name:sec10 ..." */
int speaker_builtin_hidden(const char *name);
int speaker_hide_builtin(const char *name);   /* "delete" built-in */
int speaker_restore_builtins(void);
int speaker_play_sys(const char *name);       /* system voice prompt */
void speaker_say_ip(int interrupt);            /* LEO build24: IP address EN + CZ */
void speaker_boot_announce_start(void);        /* LEO build25: "now running leotech fw" once per new fw, Wi-Fi help */
