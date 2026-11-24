/* LEO build13: user sounds on the CONFIG jffs2 partition, stored as IMA ADPCM
 * (4 bit/sample = 4 kB/s) so ~30 s fit into the free space.  Included at the
 * end of speaker.c (needs speaker_enqueue / g_generation / valid_name). */
#include <sys/statvfs.h>
#include <sys/stat.h>

static const int adpcm_index_tab[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };
static const int adpcm_step_tab[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767 };

typedef struct { int pred, idx; } adpcm_st;

static int adpcm_dec1(adpcm_st *s, int code)
{
    int step = adpcm_step_tab[s->idx], diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8) s->pred -= diff; else s->pred += diff;
    if (s->pred > 32767) s->pred = 32767; else if (s->pred < -32768) s->pred = -32768;
    s->idx += adpcm_index_tab[code & 15];
    if (s->idx < 0) s->idx = 0; else if (s->idx > 88) s->idx = 88;
    return s->pred;
}

static int adpcm_enc1(adpcm_st *s, int sample)
{
    int step = adpcm_step_tab[s->idx], diff = sample - s->pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) { code |= 1; }
    adpcm_dec1(s, code);            /* keep encoder state identical to decoder */
    return code;
}

static int user_path(const char *name, char *out, size_t cap)
{
    if (!valid_name(name)) return -1;
    snprintf(out, cap, SPEAKER_USER_DIR "/%s.adp", name);
    return 0;
}

int speaker_is_builtin(const char *name)
{
    if (!valid_name(name)) return 0;
    char p[96]; snprintf(p, sizeof p, SPEAKER_SOUND_DIR "/%s.s16", name);
    return access(p, F_OK) == 0;
}

/* file: "LADP" u32 nsamples, then nibbles (low nibble first) */
static int play_adp_path(const char *p, const char *name)
{
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    unsigned char hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "LADP", 4)) { fclose(f); return -1; }
    uint32_t n = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    adpcm_st st = { 0, 0 };
    unsigned gen = g_generation;
    int16_t out[1024]; size_t o = 0; uint32_t done = 0;
    int c;
    while (done < n && (c = fgetc(f)) != EOF) {
        out[o++] = (int16_t)adpcm_dec1(&st, c & 15); done++;
        if (done < n) { out[o++] = (int16_t)adpcm_dec1(&st, (c >> 4) & 15); done++; }
        if (o >= 1022) { if (speaker_enqueue(out, o, 0, gen) < o) { o = 0; break; } o = 0; }
    }
    if (o) speaker_enqueue(out, o, 0, gen);
    fclose(f);
    printf("[speaker] play %s (%u samples)\n", name, n);
    return 0;
}

static int play_user(const char *name)
{
    char p[96]; if (user_path(name, p, sizeof p)) return -1;
    return play_adp_path(p, name);
}

int speaker_push(const int16_t *s, size_t n)
{
    speaker_stop();
    return (int)speaker_enqueue(s, n, 0, g_generation);
}

int speaker_save_user(const char *name, const int16_t *s, size_t n)
{
    char p[96], tmp[100];
    if (user_path(name, p, sizeof p)) return -EINVAL;
    if (speaker_is_builtin(name)) return -EEXIST;
    if (n == 0 || n > SPEAKER_USER_MAX_SAMPLES) return -E2BIG;
    mkdir(SPEAKER_USER_DIR, 0755);
    long long need = 8 + (n + 1) / 2 + 8192;  /* + jffs2 slack */
    if (speaker_user_free_bytes() < need) return -ENOSPC;
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb"); if (!f) return -errno;
    unsigned char hdr[8] = { 'L','A','D','P', n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255 };
    fwrite(hdr, 1, 8, f);
    adpcm_st st = { 0, 0 };
    for (size_t i = 0; i < n; i += 2) {
        int lo = adpcm_enc1(&st, s[i]);
        int hi = (i + 1 < n) ? adpcm_enc1(&st, s[i + 1]) : 0;
        fputc(lo | hi << 4, f);
    }
    int err = ferror(f);
    fflush(f); fsync(fileno(f)); fclose(f);
    if (err || rename(tmp, p) != 0) { unlink(tmp); sync(); return -ENOSPC; }
    sync();
    printf("[speaker] saved user sound %s (%zu samples)\n", name, n);
    return 0;
}

int speaker_delete_user(const char *name)
{
    char p[96]; if (user_path(name, p, sizeof p)) return -EINVAL;
    if (unlink(p) != 0) return -errno;
    sync();
    return 0;
}

long long speaker_user_free_bytes(void)
{
    struct statvfs v;
    if (statvfs("/etc/config", &v) != 0) return 0;
    return (long long)v.f_bavail * v.f_bsize;
}

/* user list: "name:seconds_x10 ..." */
int speaker_list_user(char *out, size_t cap)
{
    out[0] = 0;
    DIR *d = opendir(SPEAKER_USER_DIR); if (!d) return 0;
    struct dirent *e; int n = 0; size_t len = 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".adp")) continue;
        char nm[64]; if (l - 4 >= sizeof nm) continue;
        memcpy(nm, e->d_name, l - 4); nm[l - 4] = 0;
        char p[128]; snprintf(p, sizeof p, SPEAKER_USER_DIR "/%s", e->d_name);
        struct stat st; long ds = 0;
        if (stat(p, &st) == 0 && st.st_size > 8) ds = (long)((st.st_size - 8) * 2 * 10 / 8000);
        int w = snprintf(out + len, cap - len, "%s%s:%ld", len ? " " : "", nm, ds);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += w; n++;
    }
    closedir(d);
    return n;
}

/* LEO build15: built-in sounds (read-only squashfs) can be "deleted" = hidden
 * via /etc/config/sounds/hidden (one name per line); restore clears the list. */
#define SPEAKER_HIDDEN_FILE SPEAKER_USER_DIR "/hidden"
int speaker_builtin_hidden(const char *name)
{
    FILE *f = fopen(SPEAKER_HIDDEN_FILE, "r"); if (!f) return 0;
    char line[64]; int hit = 0;
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (!strcmp(line, name)) { hit = 1; break; } }
    fclose(f);
    return hit;
}

int speaker_hide_builtin(const char *name)
{
    if (!speaker_is_builtin(name)) return -ENOENT;
    if (speaker_builtin_hidden(name)) return 0;
    mkdir(SPEAKER_USER_DIR, 0755);
    FILE *f = fopen(SPEAKER_HIDDEN_FILE, "a"); if (!f) return -errno;
    fprintf(f, "%s\n", name); fflush(f); fsync(fileno(f)); fclose(f); sync();
    return 0;
}

int speaker_restore_builtins(void) { unlink(SPEAKER_HIDDEN_FILE); sync(); return 0; }
