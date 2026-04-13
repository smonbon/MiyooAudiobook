// ============================================================
//  MiyooAudiobook – Audiobook player for Miyoo Mini+ / OnionOS
//  Three-level: Artists -> Albums -> Tracks
//  Flat data layout to stay under ~1MB total RAM
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <math.h>

#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#include <SDL/SDL_ttf.h>
#include <SDL/SDL_image.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>

#define SCREEN_W    640
#define SCREEN_H    480
#define FPS         30

#define AUDIOBOOK_DIR_DEFAULT  "/mnt/SDCARD/Audiobooks"
#define SAVE_FILE      "/mnt/SDCARD/App/MiyooAudiobook/progress.txt"
#define FONT_PATH      "/mnt/SDCARD/App/MiyooAudiobook/assets/font.ttf"
#define LOG_PATH       "/mnt/SDCARD/App/MiyooAudiobook/debug.log"
#define CURL_PATH      "/mnt/SDCARD/.tmp_update/bin/curl"

#define MAX_TRACKS    4096
#define MAX_ALBUMS    128
#define MAX_ARTISTS   32
#define MAX_PATH_LEN  512
#define VOL_STEP      8
#define VOL_MAX       MIX_MAX_VOLUME
#define KEY_REPEAT_DELAY    400
#define KEY_REPEAT_INTERVAL 80
#define SCREEN_OFF_DELAY    10000   // 10 seconds (default, overridden by settings)
#define SETTINGS_FILE  "/mnt/SDCARD/App/MiyooAudiobook/settings.txt"

// Backlight via PWM (Miyoo Mini+)
#define PWM_DUTY   "/sys/class/pwm/pwmchip0/pwm0/duty_cycle"
#define PWM_ENABLE "/sys/class/pwm/pwmchip0/pwm0/enable"


// ============================================================
//  Backlight control
// ============================================================

static int g_screen_off = 0;
static Uint32 g_last_input = 0;
static int g_saved_brightness = 0;  // saved duty_cycle to restore
static int g_pending_seek_ms = -1;  // deferred seek to debounce rapid presses
static int g_tz_offset_sec = 0;     // UTC offset in seconds (auto-detected)


static void write_file(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(val, f); fclose(f); }
}

static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int val = 0;
    if (fscanf(f, "%d", &val) != 1) val = 0;
    fclose(f);
    return val;
}

static void set_backlight(const char *val) {
    write_file(PWM_DUTY, val);
}

static void screen_on(void) {
    if (g_screen_off) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", g_saved_brightness);
        set_backlight(buf);
        g_screen_off = 0;
    }
    g_last_input = SDL_GetTicks();
}

static void screen_off(void) {
    if (!g_screen_off) {
        // Save current brightness before turning off
        int cur = read_int_file(PWM_DUTY);
        if (cur > 0) g_saved_brightness = cur;
        set_backlight("0");
        g_screen_off = 1;
    }
}

// ============================================================
//  Stay-awake (prevent OnionOS auto-sleep while playing)
// ============================================================

#define STAY_AWAKE_PATH "/tmp/stay_awake"

static void stay_awake_set(int on) {
    if (on) {
        FILE *f = fopen(STAY_AWAKE_PATH, "w");
        if (f) fclose(f);
    } else {
        unlink(STAY_AWAKE_PATH);
    }
}

// ============================================================
//  Debug logging
// ============================================================

static FILE *g_log = NULL;
static void log_init(void) {
    g_log = fopen(LOG_PATH, "w");
    if (g_log) fprintf(g_log, "=== MiyooAudiobook Log ===\n");
}
static void log_msg(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fprintf(g_log, "\n"); fflush(g_log);
}
static void log_close(void) {
    if (g_log) { fprintf(g_log, "=== END ===\n"); fclose(g_log); g_log = NULL; }
}

// ============================================================
//  Flat data structures (~900KB total instead of ~400MB)
// ============================================================

typedef struct {
    char filepath[MAX_PATH_LEN];
    char title[256];
    int  duration_ms;   // estimated from MP3 header
} Track;

typedef struct {
    char title[256];
    char dirpath[MAX_PATH_LEN];
    int  track_start;       // index into g_tracks[]
    int  track_count;
    int  saved_track;
    int  saved_position_ms;
} Album;

typedef struct {
    char title[256];
    int  album_start;       // index into g_albums[]
    int  album_count;
} Artist;

// Global flat arrays
static Track  g_tracks[MAX_TRACKS];
static int    g_track_total = 0;
static Album  g_albums[MAX_ALBUMS];
static int    g_album_total = 0;
static Artist g_artists[MAX_ARTISTS];
static int    g_artist_total = 0;

typedef enum { VIEW_ARTISTS, VIEW_ALBUMS, VIEW_TRACKS, VIEW_NOWPLAYING } ViewMode;

typedef struct {
    ViewMode view;
    int      selected;
    int      scroll_offset;
    int      cur_artist;
    int      cur_album;     // index into g_albums[]

    int      play_artist;
    int      play_album;    // index into g_albums[]
    int      play_track;    // index into g_tracks[]
    int      is_paused;
    Uint32   play_start_ticks;
    int      play_start_offset_ms;

    int      volume;
    char     error_msg[128];
    int      audio_ok;

    Mix_Music   *music;
    SDL_Surface *screen;
    SDL_Surface *cover_art;       // cached album cover (NULL if none)
    int          cover_album_id;  // which album the cached cover belongs to
    TTF_Font    *font_large;
    TTF_Font    *font_small;
    int          running;
    int          visible_rows;

    // Resume prompt
    int          show_resume;
    int          resume_album;
    int          resume_artist;

    // Quit confirmation
    int          show_quit_confirm;

    // Speed control (index into speed_values[])
    int          speed_idx;

    // Saved browse view for Start toggle
    ViewMode     saved_view;
    int          saved_selected;

    // Lock mechanism
    int          locked;            // 1 = input locked
    Uint32       lock_combo_since;  // ticks when L2+R2 first held together
    int          lock_confirm;      // 1 = showing "Unlock?" confirmation

    // Sleep timer
    int          sleep_timer_min;   // 0 = off, else minutes until pause
    Uint32       sleep_timer_start; // ticks when timer started

    // Title scroll state (now playing)
    int          title_scroll_x;      // current pixel offset
    int          title_scroll_w;      // total text width (0 = no scroll needed)
    int          title_scroll_max;    // max visible width
    Uint32       title_scroll_wait;   // tick when scroll pauses (start or end)
    int          title_scroll_state;  // 0=waiting start, 1=scrolling, 2=waiting end
    int          title_scroll_track;  // track index we're scrolling for
} AppState;

// Speed options
static const float speed_values[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
static const char *speed_labels[] = { "0.5x", "0.75x", "1x", "1.25x", "1.5x", "1.75x", "2x" };
#define SPEED_COUNT 7
#define SPEED_DEFAULT 2  // index of 1.0x

static AppState app;

// ============================================================
//  Manual MP3 decoding via mpg123 (loaded with dlopen)
//  Enables variable-speed playback via Mix_HookMusic.
// ============================================================

#include <dlfcn.h>
#include <mpg123.h>

// Function pointers for mpg123 (loaded via dlopen to avoid glibc version issues)
static int (*mp3_init)(void);
static void (*mp3_exit)(void);
static mpg123_handle *(*mp3_new)(const char *, int *);
static void (*mp3_delete)(mpg123_handle *);
static int (*mp3_open)(mpg123_handle *, const char *);
static int (*mp3_close)(mpg123_handle *);
static int (*mp3_getformat)(mpg123_handle *, long *, int *, int *);
static int (*mp3_format_none)(mpg123_handle *);
static int (*mp3_format)(mpg123_handle *, long, int, int);
static int (*mp3_read)(mpg123_handle *, unsigned char *, size_t, size_t *);
static off_t (*mp3_seek)(mpg123_handle *, off_t, int);
static off_t (*mp3_tell)(mpg123_handle *);

static void *g_mpg123_lib = NULL;
static mpg123_handle *g_decoder = NULL;
static long g_decode_rate = 44100;
static int g_decode_channels = 2;
static int g_music_done = 0;

// Ring buffer for decoded PCM (stereo 16-bit frames)
#define RING_FRAMES 65536
static Sint16 g_ring[RING_FRAMES * 2];  // stereo
static int g_ring_wpos = 0;
static int g_ring_rpos = 0;
static int g_ring_avail = 0;
static long g_frames_played = 0;  // total source frames consumed (for position)
static long g_seek_base_frames = 0;  // sample offset after last seek

// WSOLA time-stretching (pitch-preserving speed change)
// Larger windows + search range reduce robotic artifacts for speech
#define WSOLA_WINDOW   2048   // ~46ms at 44100Hz — captures multiple pitch periods
#define WSOLA_SYNHOP   1024   // synthesis hop = window/2 (50% overlap)
#define WSOLA_SEARCH   512    // search range ±512 frames (~11.6ms)
#define WSOLA_BUFSIZE  8192   // output buffer size

static Sint16 g_wsola_buf[WSOLA_BUFSIZE * 2];  // stereo output ring
static int g_wsola_rpos = 0;
static int g_wsola_wpos = 0;
static int g_wsola_avail = 0;
static Sint16 g_wsola_prev[WSOLA_WINDOW * 2];  // previous window for overlap
static int g_wsola_has_prev = 0;
static Sint16 g_wsola_newwin[WSOLA_WINDOW * 2]; // scratch buffer for new window

static int load_mpg123(void) {
    g_mpg123_lib = dlopen("libmpg123.so.0", RTLD_NOW);
    if (!g_mpg123_lib) {
        log_msg("dlopen mpg123 failed: %s", dlerror());
        return 0;
    }
    mp3_init        = dlsym(g_mpg123_lib, "mpg123_init");
    mp3_exit        = dlsym(g_mpg123_lib, "mpg123_exit");
    mp3_new         = dlsym(g_mpg123_lib, "mpg123_new");
    mp3_delete      = dlsym(g_mpg123_lib, "mpg123_delete");
    mp3_open        = dlsym(g_mpg123_lib, "mpg123_open");
    mp3_close       = dlsym(g_mpg123_lib, "mpg123_close");
    mp3_getformat   = dlsym(g_mpg123_lib, "mpg123_getformat");
    mp3_format_none = dlsym(g_mpg123_lib, "mpg123_format_none");
    mp3_format      = dlsym(g_mpg123_lib, "mpg123_format");
    mp3_read        = dlsym(g_mpg123_lib, "mpg123_read");
    mp3_seek        = dlsym(g_mpg123_lib, "mpg123_seek");
    mp3_tell        = dlsym(g_mpg123_lib, "mpg123_tell");

    if (!mp3_init || !mp3_new || !mp3_open || !mp3_read) {
        log_msg("mpg123 symbols missing");
        dlclose(g_mpg123_lib);
        g_mpg123_lib = NULL;
        return 0;
    }
    mp3_init();
    log_msg("mpg123 loaded OK (dlopen, our build)");
    return 1;
}

// Decode MP3 data into ring buffer
static void fill_ring(void) {
    while (g_ring_avail < RING_FRAMES * 3 / 4 && g_decoder && !g_music_done) {
        unsigned char buf[8192];
        size_t done = 0;
        int ret = mp3_read(g_decoder, buf, sizeof(buf), &done);

        if (done > 0) {
            Sint16 *src = (Sint16 *)buf;
            int src_frames = done / (2 * g_decode_channels);
            for (int i = 0; i < src_frames; i++) {
                if (g_ring_avail >= RING_FRAMES) break;  // ring full
                int wp = g_ring_wpos % RING_FRAMES;
                if (g_decode_channels == 2) {
                    g_ring[wp * 2]     = src[i * 2];
                    g_ring[wp * 2 + 1] = src[i * 2 + 1];
                } else {
                    g_ring[wp * 2]     = src[i];
                    g_ring[wp * 2 + 1] = src[i];
                }
                g_ring_wpos = (g_ring_wpos + 1) % RING_FRAMES;
                g_ring_avail++;
            }
        }
        if (ret == MPG123_DONE) { g_music_done = 1; break; }
        if (done == 0) break;
    }
}

// WSOLA: write one stereo frame to output buffer
static inline void wsola_write(Sint16 l, Sint16 r) {
    if (g_wsola_avail >= WSOLA_BUFSIZE) return;
    g_wsola_buf[g_wsola_wpos * 2] = l;
    g_wsola_buf[g_wsola_wpos * 2 + 1] = r;
    g_wsola_wpos = (g_wsola_wpos + 1) % WSOLA_BUFSIZE;
    g_wsola_avail++;
}

// WSOLA: time-stretch from source ring buffer into output buffer
static void wsola_process(void) {
    float speed = speed_values[app.speed_idx];
    int ana_hop = (int)(WSOLA_SYNHOP * speed + 0.5f);
    if (ana_hop < 1) ana_hop = 1;

    // Speed ~1.0: bypass WSOLA, direct copy
    if (speed >= 0.99f && speed <= 1.01f) {
        while (g_wsola_avail < WSOLA_BUFSIZE * 3 / 4 && g_ring_avail > 0) {
            int ri = g_ring_rpos * 2;
            wsola_write(g_ring[ri], g_ring[ri + 1]);
            g_ring_rpos = (g_ring_rpos + 1) % RING_FRAMES;
            g_ring_avail--;
            g_frames_played++;
        }
        return;
    }

    // Limit search range to ±25% of ana_hop to prevent speed drift
    int search = WSOLA_SEARCH;
    if (search > ana_hop / 2) search = ana_hop / 2;
    if (search < 16) search = 16;

    int needed = ana_hop + WSOLA_WINDOW + search;

    while (g_wsola_avail < WSOLA_BUFSIZE * 3 / 4) {
        // Flush remaining data when source is exhausted
        if (g_music_done && g_ring_avail > 0 && g_ring_avail < needed) {
            while (g_ring_avail > 0 && g_wsola_avail < WSOLA_BUFSIZE) {
                int ri = g_ring_rpos * 2;
                wsola_write(g_ring[ri], g_ring[ri + 1]);
                g_ring_rpos = (g_ring_rpos + 1) % RING_FRAMES;
                g_ring_avail--;
                g_frames_played++;
            }
            break;
        }

        if (g_ring_avail < needed) break;

        if (!g_wsola_has_prev) {
            // First chunk: copy window, output first half
            for (int i = 0; i < WSOLA_WINDOW; i++) {
                int ri = ((g_ring_rpos + i) % RING_FRAMES) * 2;
                g_wsola_prev[i * 2]     = g_ring[ri];
                g_wsola_prev[i * 2 + 1] = g_ring[ri + 1];
                if (i < WSOLA_SYNHOP)
                    wsola_write(g_ring[ri], g_ring[ri + 1]);
            }
            g_ring_rpos = (g_ring_rpos + ana_hop) % RING_FRAMES;
            g_ring_avail -= ana_hop;
            g_frames_played += ana_hop;
            g_wsola_has_prev = 1;
            continue;
        }

        // Find best overlap offset via cross-correlation
        // Compare tail of previous window with candidates near ana_hop
        Sint16 *prev_tail = &g_wsola_prev[WSOLA_SYNHOP * 2];
        int best_off = 0;
        long best_corr = -2147483647L;

        for (int off = -search; off <= search; off += 2) {
            int cand = ana_hop + off;
            if (cand < 0 || cand + WSOLA_WINDOW > g_ring_avail) continue;

            long corr = 0;
            for (int i = 0; i < WSOLA_SYNHOP; i += 8) {
                int ri = ((g_ring_rpos + cand + i) % RING_FRAMES) * 2;
                corr += (long)prev_tail[i * 2]     * g_ring[ri];
                corr += (long)prev_tail[i * 2 + 1] * g_ring[ri + 1];
            }
            if (corr > best_corr) {
                best_corr = corr;
                best_off = off;
            }
        }

        int win_start = ana_hop + best_off;
        if (win_start < 0) win_start = 0;

        // Read new window
        for (int i = 0; i < WSOLA_WINDOW; i++) {
            int ri = ((g_ring_rpos + win_start + i) % RING_FRAMES) * 2;
            g_wsola_newwin[i * 2]     = g_ring[ri];
            g_wsola_newwin[i * 2 + 1] = g_ring[ri + 1];
        }

        // Crossfade with Hann window (smoother than linear, less robotic)
        for (int i = 0; i < WSOLA_SYNHOP; i++) {
            float w = 0.5f * (1.0f - cosf((float)M_PI * i / WSOLA_SYNHOP));
            Sint16 l = (Sint16)(prev_tail[i * 2]     * (1.0f - w) + g_wsola_newwin[i * 2]     * w);
            Sint16 r = (Sint16)(prev_tail[i * 2 + 1] * (1.0f - w) + g_wsola_newwin[i * 2 + 1] * w);
            wsola_write(l, r);
        }

        // Save new window as prev for next iteration
        memcpy(g_wsola_prev, g_wsola_newwin, WSOLA_WINDOW * 2 * sizeof(Sint16));

        // Advance source by EXACTLY ana_hop (deterministic speed)
        // The search offset only affects window placement, not consumption rate
        int consume = (ana_hop > g_ring_avail) ? g_ring_avail : ana_hop;
        g_ring_rpos = (g_ring_rpos + consume) % RING_FRAMES;
        g_ring_avail -= consume;
        g_frames_played += ana_hop;
    }
}

// SDL audio callback — fills buffer from WSOLA pipeline
static void audio_callback(void *udata, Uint8 *stream, int len) {
    (void)udata;
    Sint16 *out = (Sint16 *)stream;
    int out_frames = len / 4;
    if (app.is_paused || !g_decoder) {
        memset(stream, 0, len);
        return;
    }

    fill_ring();
    wsola_process();

    float vol = (float)app.volume / (float)MIX_MAX_VOLUME;
    int produced = 0;

    for (int i = 0; i < out_frames; i++) {
        if (g_wsola_avail > 0) {
            out[i * 2]     = (Sint16)(g_wsola_buf[g_wsola_rpos * 2]     * vol);
            out[i * 2 + 1] = (Sint16)(g_wsola_buf[g_wsola_rpos * 2 + 1] * vol);
            g_wsola_rpos = (g_wsola_rpos + 1) % WSOLA_BUFSIZE;
            g_wsola_avail--;
            produced++;
        } else {
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
        }
    }
}

static void decoder_close(void);

// Open an MP3 for decoding
static int decoder_open(const char *filepath, int seek_ms) {
    // Close any existing decoder (audio callback sees g_decoder=NULL → silence)
    decoder_close();

    // Prepare new decoder WITHOUT exposing it to the audio callback yet
    int err = 0;
    mpg123_handle *dec = mp3_new(NULL, &err);
    if (!dec) { log_msg("mpg123_new failed: %d", err); return 0; }

    mp3_format_none(dec);
    mp3_format(dec, 44100, MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mp3_format(dec, 44100, MPG123_MONO, MPG123_ENC_SIGNED_16);

    if (mp3_open(dec, filepath) != MPG123_OK) {
        log_msg("mpg123_open failed: %s", filepath);
        mp3_delete(dec);
        return 0;
    }

    long rate = 44100; int channels = 2, encoding = 0;
    if (mp3_getformat(dec, &rate, &channels, &encoding) != MPG123_OK) {
        log_msg("  mpg123_getformat failed, assuming 44100 Hz stereo");
        rate = 44100; channels = 2;
    }
    if (rate < 8000 || rate > 48000) rate = 44100;
    if (channels < 1 || channels > 2) channels = 2;
    log_msg("  mpg123 format: %ld Hz, %d ch", rate, channels);

    // Seek (can be slow on large files — done before exposing to callback)
    long seek_base = 0;
    if (seek_ms > 0) {
        long sample_off = (long)((long long)seek_ms * rate / 1000);
        mp3_seek(dec, sample_off, 0 /*SEEK_SET*/);
        seek_base = sample_off;
    }

    // Pre-fill ring buffer using local decoder handle (callback can't see it yet)
    g_decode_rate = rate;
    g_decode_channels = channels;
    g_ring_wpos = 0;
    g_ring_rpos = 0;
    g_ring_avail = 0;
    g_frames_played = 0;
    g_music_done = 0;
    g_wsola_rpos = 0;
    g_wsola_wpos = 0;
    g_wsola_avail = 0;
    g_wsola_has_prev = 0;

    // Temporarily set g_decoder for fill_ring (audio is locked so callback won't run)
    SDL_LockAudio();
    g_decoder = dec;
    g_seek_base_frames = seek_base;
    fill_ring();
    SDL_UnlockAudio();
    // Now audio callback can see g_decoder and fully initialized buffers

    log_msg("  decoder ready, playback started (ring=%d)", g_ring_avail);
    return 1;
}

static void decoder_close(void) {
    SDL_LockAudio();
    if (g_decoder) {
        mp3_close(g_decoder);
        mp3_delete(g_decoder);
        g_decoder = NULL;
    }
    g_music_done = 0;
    g_ring_avail = 0;
    g_wsola_avail = 0;
    g_wsola_has_prev = 0;
    SDL_UnlockAudio();
}

// Seek to absolute position in ms
static void decoder_seek(int ms) {
    if (!g_decoder) return;
    // Lock audio to prevent music_hook from reading stale buffer state
    SDL_LockAudio();
    long sample_off = (long)((long long)ms * g_decode_rate / 1000);
    mp3_seek(g_decoder, sample_off, 0 /*SEEK_SET*/);
    g_seek_base_frames = sample_off;
    g_ring_wpos = 0;
    g_ring_rpos = 0;
    g_ring_avail = 0;
    g_frames_played = 0;
    g_music_done = 0;
    g_wsola_rpos = 0;
    g_wsola_wpos = 0;
    g_wsola_avail = 0;
    g_wsola_has_prev = 0;
    fill_ring();
    SDL_UnlockAudio();
}

// Current playback position in ms
static int decoder_pos_ms(void) {
    if (!g_decoder) return 0;
    return (int)((g_seek_base_frames + g_frames_played) * 1000LL / g_decode_rate);
}

// Screen timeout options (ms, 0=never)
static const int timeout_values[] = { 5000, 10000, 15000, 30000, 60000, 0 };
static const char *timeout_labels[] = { "5s", "10s", "15s", "30s", "60s", "Never" };
#define TIMEOUT_COUNT 6
#define TIMEOUT_DEFAULT 1  // 10s

// Sleep timer options (minutes, 0=off)
static const int sleep_values[] = { 0, 5, 10, 15, 20, 25, 30, 45, 60 };
static const char *sleep_labels_en[] = { "Off", "5min", "10min", "15min", "20min", "25min", "30min", "45min", "60min" };
static const char *sleep_labels_de[] = { "Aus", "5min", "10min", "15min", "20min", "25min", "30min", "45min", "60min" };
#define SLEEP_LABEL(i) (g_lang_idx == LANG_EN ? sleep_labels_en[i] : sleep_labels_de[i])
#define SLEEP_COUNT 9

// Language support
#define LANG_EN 0
#define LANG_DE 1
#define LANG_COUNT 2
static const char *lang_labels[] = { "English", "Deutsch" };

// All translatable strings
typedef struct {
    const char *settings;
    const char *screen_timeout;
    const char *sleep_timer;
    const char *language;
    const char *continue_listening;
    const char *quit_title;
    const char *quit_yes;
    const char *quit_no;
    const char *nothing_playing;
    const char *lock_hint;
    const char *locked_msg;
    const char *unlock_msg;
    const char *unlock_hold;
    const char *audiobook_pct;
    const char *track_pct;
    const char *sleep_in;
    const char *for_miyoo;
    const char *hint_artists;
    const char *hint_albums;
    const char *hint_tracks;
    const char *np_pause;
    const char *np_play;
    const char *np_prev_next;
    const char *np_speed;
    const char *np_browse_back;
    const char *np_track;
    const char *lock_confirm;
    const char *lock_cancel;
    const char *unlock_confirm;
    const char *unlock_cancel;
    const char *unlock_to_unlock;
    const char *opts_back;
    const char *lock_screen_q;
    const char *unlock_screen_q;
    const char *reset_progress;
    const char *reset_done;
    const char *reset_confirm_q;
    const char *source_path;
    const char *opts_hint_action;
    const char *download_covers;
    const char *cover_searching;
    const char *cover_downloading;
    const char *cover_done;
    const char *cover_result;
    const char *cover_no_wifi;
    const char *press_b_cancel;
    const char *clock_format;
    const char *about;
} Strings;

static const Strings strings[LANG_COUNT] = {
    { // English
        "Settings", "Screen timeout", "Sleep timer", "Language",
        "Continue listening?", "Quit Audiobooks?",
        "A: Yes, quit", "B: No, go back",
        "Nothing playing",
        "Hold L2+R2: Lock", "Locked", "Unlock?",
        "Hold L2+R2, then A",
        "Audiobook: %d%%", "Track: %d%%", "Sleep in %d:%02d",
        "for Miyoo Mini+",
        "A:Open  Start:NowPlaying  Select:Settings  B:Quit",
        "A:Open  Start:NowPlaying  Select:Settings  B:Back",
        "A:Play  Start:NowPlaying  Select:Settings  B:Back",
        "A:Pause", "A:Play",
        "Left:Prev  Right:Next", "Up/Dn:Speed",
        "Start:Browse  B:Back", "Track %d / %d",
        "A: Confirm lock", "B: Cancel",
        "A: Confirm unlock", "B: Cancel",
        "Hold L2+R2, then press A to unlock",
        "Up/Dn:Select  L/R:Change  Select:Back",
        "Lock screen?", "Unlock screen?",
        "Reset all progress", "Progress reset!",
        "Reset all progress? A:Yes  B:No",
        "Audiobooks path", "A:Action  L/R:Change  Select:Back",
        "Download covers", "Searching %s...",
        "Downloading cover...", "Cover search complete",
        "%d of %d covers found", "No network connection",
        "B: Cancel",
        "Clock format",
        "About"
    },
    { // Deutsch
        "Einstellungen", "Bildschirm-Timeout", "Schlaf-Timer", "Sprache",
        "Weiterhören?", "App beenden?",
        "A: Ja, beenden", "B: Nein, zurück",
        "Nichts aktiv",
        "L2+R2 halten: Sperren", "Gesperrt", "Entsperren?",
        "L2+R2 halten, dann A",
        "Hörbuch: %d%%", "Titel: %d%%", "Schlaf in %d:%02d",
        "für Miyoo Mini+",
        "A:Öffnen  Start:Wiedergabe  Select:Einst.  B:Beenden",
        "A:Öffnen  Start:Wiedergabe  Select:Einst.  B:Zurück",
        "A:Abspielen  Start:Wiedergabe  Select:Einst.  B:Zurück",
        "A:Pause", "A:Abspielen",
        "Links:Zurück  Rechts:Weiter", "Auf/Ab:Tempo",
        "Start:Übersicht  B:Zurück", "Titel %d / %d",
        "A: Sperren bestät.", "B: Abbrechen",
        "A: Entsperr. bestät.", "B: Abbrechen",
        "L2+R2 halten, dann A drücken zum Entsperren",
        "Auf/Ab:Wahl  L/R:Ändern  Select:Zurück",
        "Bildschirm sperren?", "Bildschirm entsperren?",
        "Alle Fortschritte zurücksetzen", "Fortschritt zurückgesetzt!",
        "Alle Fortschritte zurücksetzen? A:Ja  B:Nein",
        "Hörbuch-Pfad", "A:Aktion  L/R:Ändern  Select:Zurück",
        "Cover herunterladen", "Suche %s...",
        "Lade Cover herunter...", "Cover-Suche abgeschlossen",
        "%d von %d Covers gefunden", "Keine Netzwerkverbindung",
        "B: Abbrechen",
        "Uhrzeitformat",
        "Info"
    }
};

// Audiobook source path options
static const char *path_options[] = {
    "/mnt/SDCARD/Audiobooks",
    "/mnt/SDCARD/Media/Audiobooks",
    "/mnt/SDCARD/Media/Books",
    "/mnt/SDCARD/Books",
    "/mnt/SDCARD/Audio",
};
#define PATH_COUNT 5
static int g_path_idx = 0;   // index into path_options

// Settings
static int g_timeout_idx = TIMEOUT_DEFAULT;
static int g_sleep_idx = 0;
static int g_lang_idx = LANG_EN;
static int g_show_options = 0;
static int g_option_selected = 0;
static int g_reset_confirm = 0;   // 1 = showing "are you sure?" flash
static Uint32 g_reset_flash_until = 0;
static int g_clock_24h = 1;  // 1 = 24h format, 0 = 12h AM/PM
static int g_show_about = 0;
#define OPTION_COUNT 8  // timeout, sleep, language, path, clock_fmt, covers, reset, about

static const Strings *S(void) { return &strings[g_lang_idx]; }

static void save_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "screen_timeout=%d\n", g_timeout_idx);
    fprintf(f, "sleep_timer=%d\n", g_sleep_idx);
    fprintf(f, "language=%d\n", g_lang_idx);
    fprintf(f, "path_idx=%d\n", g_path_idx);
    fprintf(f, "volume=%d\n", app.volume);
    fprintf(f, "speed=%d\n", app.speed_idx);
    fprintf(f, "clock_24h=%d\n", g_clock_24h);
    fclose(f);
}

static void load_settings(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, "screen_timeout=%d", &val) == 1 && val >= 0 && val < TIMEOUT_COUNT)
            g_timeout_idx = val;
        else if (sscanf(line, "sleep_timer=%d", &val) == 1 && val >= 0 && val < SLEEP_COUNT)
            g_sleep_idx = val;
        else if (sscanf(line, "language=%d", &val) == 1 && val >= 0 && val < LANG_COUNT)
            g_lang_idx = val;
        else if (sscanf(line, "path_idx=%d", &val) == 1 && val >= 0 && val < PATH_COUNT)
            g_path_idx = val;
        else if (sscanf(line, "speed=%d", &val) == 1 && val >= 0 && val < SPEED_COUNT)
            app.speed_idx = val;
        else if (sscanf(line, "volume=%d", &val) == 1 && val >= 0 && val <= VOL_MAX)
            app.volume = val;
        else if (sscanf(line, "clock_24h=%d", &val) == 1 && (val == 0 || val == 1))
            g_clock_24h = val;
    }
    fclose(f);
}

static void stop_playback(void);  // forward decl

static void do_reset_progress(void) {
    // Stop any current playback
    stop_playback();
    // Clear all in-memory album progress
    for (int i = 0; i < g_album_total; i++) {
        g_albums[i].saved_track = 0;
        g_albums[i].saved_position_ms = 0;
    }
    // Delete the save file
    remove(SAVE_FILE);
    // Clear resume dialog state
    app.show_resume = 0;
    app.resume_album = -1;
    app.resume_artist = -1;
    g_reset_flash_until = SDL_GetTicks() + 2000;
    log_msg("Progress reset");
}

// ============================================================
//  Helpers
// ============================================================

static SDL_Color rgb(Uint8 r, Uint8 g, Uint8 b) {
    SDL_Color c = { r, g, b, 0 }; return c;
}

static void draw_text(TTF_Font *font, const char *text,
                      SDL_Color color, int x, int y) {
    if (!font || !text || !text[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Solid(font, text, color);
    if (!surf) return;
    SDL_Rect dst = { x, y, 0, 0 };
    SDL_BlitSurface(surf, NULL, app.screen, &dst);
    SDL_FreeSurface(surf);
}

static void fill_rect(int x, int y, int w, int h, Uint32 color) {
    SDL_Rect r = { x, y, w, h };
    SDL_FillRect(app.screen, &r, color);
}

static void draw_text_right(TTF_Font *font, const char *text,
                            SDL_Color color, int right_x, int y) {
    if (!font || !text || !text[0]) return;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    draw_text(font, text, color, right_x - w, y);
}

static void draw_text_center(TTF_Font *font, const char *text,
                             SDL_Color color, int center_x, int y) {
    if (!font || !text || !text[0]) return;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    draw_text(font, text, color, center_x - w / 2, y);
}

static void truncate_str(const char *src, char *dst, int max_chars) {
    if (max_chars < 4) { dst[0] = '\0'; return; }
    int len = (int)strlen(src);
    if (len <= max_chars) { strcpy(dst, src); return; }
    strncpy(dst, src, max_chars - 3);
    dst[max_chars - 3] = '\0';
    strcat(dst, "...");
}

static void strip_ext(const char *name, char *out, size_t max) {
    strncpy(out, name, max - 1); out[max - 1] = '\0';
    char *dot = strrchr(out, '.'); if (dot) *dot = '\0';
}

static void format_time(int ms, char *buf, size_t len) {
    int s = ms / 1000, h = s / 3600, m = (s % 3600) / 60;
    s %= 60;
    if (h > 0) snprintf(buf, len, "%d:%02d:%02d", h, m, s);
    else snprintf(buf, len, "%02d:%02d", m, s);
}

static int is_audio(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    return strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".ogg") == 0 ||
           strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".flac") == 0;
}

static int track_cmp(const void *a, const void *b) {
    return strcasecmp(((const Track*)a)->filepath, ((const Track*)b)->filepath);
}

static int album_cmp(const void *a, const void *b) {
    return strcasecmp(((const Album*)a)->title, ((const Album*)b)->title);
}

static int artist_cmp(const void *a, const void *b) {
    return strcasecmp(((const Artist*)a)->title, ((const Artist*)b)->title);
}

// ============================================================
//  MP3 duration detection (parses header, supports VBR+CBR)
// ============================================================

static int get_mp3_duration_ms(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Check for ID3v2 tag and skip it
    unsigned char buf[10];
    if (fread(buf, 1, 10, f) != 10) { fclose(f); return 0; }

    long audio_start = 0;
    if (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        audio_start = 10 + (((buf[6]&0x7F)<<21)|((buf[7]&0x7F)<<14)|
                            ((buf[8]&0x7F)<<7)|(buf[9]&0x7F));
        fseek(f, audio_start, SEEK_SET);
    } else {
        fseek(f, 0, SEEK_SET);
    }

    // Find first MPEG frame sync (0xFF 0xE0+)
    unsigned char hdr[4];
    int found = 0;
    for (int i = 0; i < 16384; i++) {
        if (fread(hdr, 1, 1, f) != 1) break;
        if (hdr[0] == 0xFF) {
            if (fread(hdr+1, 1, 3, f) != 3) break;
            if ((hdr[1] & 0xE0) == 0xE0) { found = 1; break; }
            fseek(f, -3, SEEK_CUR);
        }
    }
    if (!found) { fclose(f); return 0; }

    int version = (hdr[1] >> 3) & 3;   // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    int layer   = (hdr[1] >> 1) & 3;   // 1=LayerIII
    int br_idx  = (hdr[2] >> 4) & 0xF;
    int sr_idx  = (hdr[2] >> 2) & 3;

    // Bitrate tables (kbps)
    static const int br_v1l3[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int br_v2l3[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int sr_v1[]   = {44100,48000,32000,0};
    static const int sr_v2[]   = {22050,24000,16000,0};
    static const int sr_v25[]  = {11025,12000,8000,0};

    int bitrate = 0;
    if ((version == 3) && (layer == 1)) bitrate = br_v1l3[br_idx];
    else if (layer == 1)                bitrate = br_v2l3[br_idx];

    int srate = 0;
    if (version == 3)      srate = sr_v1[sr_idx];
    else if (version == 2) srate = sr_v2[sr_idx];
    else if (version == 0) srate = sr_v25[sr_idx];

    if (bitrate == 0 || srate == 0) { fclose(f); return 0; }

    // Check for Xing/Info VBR header
    long frame_start = ftell(f) - 4;
    int side = (version == 3) ? 32 : 17;
    fseek(f, frame_start + 4 + side, SEEK_SET);

    unsigned char tag[4];
    if (fread(tag, 1, 4, f) == 4 &&
        (memcmp(tag,"Xing",4)==0 || memcmp(tag,"Info",4)==0)) {
        unsigned char fl[4];
        if (fread(fl, 1, 4, f) == 4) {
            int flags = (fl[0]<<24)|(fl[1]<<16)|(fl[2]<<8)|fl[3];
            if (flags & 1) {  // frame count present
                unsigned char fc[4];
                if (fread(fc, 1, 4, f) == 4) {
                    int frames = (fc[0]<<24)|(fc[1]<<16)|(fc[2]<<8)|fc[3];
                    int spf = (version == 3) ? 1152 : 576;
                    long long ms = (long long)frames * spf * 1000 / srate;
                    fclose(f); return (int)ms;
                }
            }
        }
    }

    // CBR fallback
    long audio_size = filesize - audio_start;
    long long ms = (long long)audio_size * 8 / bitrate;
    fclose(f);
    return (int)ms;
}

// ============================================================
//  Recursive track scan – appends to g_tracks[]
// ============================================================

static void scan_tracks(const char *dirpath) {
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && g_track_total < MAX_TRACKS) {
        if (e->d_name[0] == '.') continue;
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dirpath, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_tracks(path);  // recurse into CD subfolders
        } else if (S_ISREG(st.st_mode) && is_audio(e->d_name)) {
            Track *t = &g_tracks[g_track_total];
            strncpy(t->filepath, path, MAX_PATH_LEN - 1);
            strip_ext(e->d_name, t->title, 256);
            t->duration_ms = get_mp3_duration_ms(path);
            g_track_total++;
        }
    }
    closedir(dir);
}

// ============================================================
//  Library scan: /Audiobooks/Artist/Album/[CD#/]tracks
// ============================================================

static void scan_library(void) {
    const char *audiobook_dir = path_options[g_path_idx];
    log_msg("Scanning %s", audiobook_dir);
    DIR *root = opendir(audiobook_dir);
    if (!root) { log_msg("Cannot open dir"); return; }

    struct dirent *art_e;
    while ((art_e = readdir(root)) != NULL && g_artist_total < MAX_ARTISTS) {
        if (art_e->d_name[0] == '.') continue;
        char art_path[MAX_PATH_LEN];
        snprintf(art_path, sizeof(art_path), "%s/%s", audiobook_dir, art_e->d_name);
        struct stat st;
        if (stat(art_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        Artist *artist = &g_artists[g_artist_total];
        strncpy(artist->title, art_e->d_name, 255);
        artist->album_start = g_album_total;
        artist->album_count = 0;

        DIR *art_dir = opendir(art_path);
        if (!art_dir) continue;

        int has_loose = 0;
        struct dirent *alb_e;
        while ((alb_e = readdir(art_dir)) != NULL && g_album_total < MAX_ALBUMS) {
            if (alb_e->d_name[0] == '.') continue;
            char alb_path[MAX_PATH_LEN];
            snprintf(alb_path, sizeof(alb_path), "%s/%s", art_path, alb_e->d_name);
            struct stat st2;
            if (stat(alb_path, &st2) != 0) continue;

            if (S_ISDIR(st2.st_mode)) {
                Album *alb = &g_albums[g_album_total];
                memset(alb, 0, sizeof(Album));
                strncpy(alb->title, alb_e->d_name, 255);
                strncpy(alb->dirpath, alb_path, MAX_PATH_LEN - 1);
                alb->track_start = g_track_total;
                alb->track_count = 0;

                int before = g_track_total;
                scan_tracks(alb_path);
                alb->track_count = g_track_total - before;

                if (alb->track_count > 0) {
                    qsort(&g_tracks[alb->track_start], alb->track_count,
                          sizeof(Track), track_cmp);
                    log_msg("  Album '%s': %d tracks", alb->title, alb->track_count);
                    g_album_total++;
                    artist->album_count++;
                } else {
                    g_track_total = before; // rollback
                }
            } else if (S_ISREG(st2.st_mode) && is_audio(alb_e->d_name)) {
                has_loose = 1;
            }
        }
        closedir(art_dir);

        // Loose tracks in artist folder → single album
        if (has_loose && artist->album_count == 0 && g_album_total < MAX_ALBUMS) {
            Album *alb = &g_albums[g_album_total];
            memset(alb, 0, sizeof(Album));
            strncpy(alb->title, art_e->d_name, 255);
            strncpy(alb->dirpath, art_path, MAX_PATH_LEN - 1);
            alb->track_start = g_track_total;
            int before = g_track_total;
            scan_tracks(art_path);
            alb->track_count = g_track_total - before;
            if (alb->track_count > 0) {
                qsort(&g_tracks[alb->track_start], alb->track_count,
                      sizeof(Track), track_cmp);
                g_album_total++;
                artist->album_count = 1;
                log_msg("  (loose tracks): %d", alb->track_count);
            } else {
                g_track_total = before;
            }
        }

        if (artist->album_count > 0) {
            if (artist->album_count > 1)
                qsort(&g_albums[artist->album_start], artist->album_count,
                      sizeof(Album), album_cmp);
            log_msg("Artist '%s': %d albums", artist->title, artist->album_count);
            g_artist_total++;
        }
    }
    closedir(root);

    if (g_artist_total > 1)
        qsort(g_artists, g_artist_total, sizeof(Artist), artist_cmp);

    log_msg("Library: %d artists, %d albums, %d tracks",
            g_artist_total, g_album_total, g_track_total);
}

// ============================================================
//  Progress Save / Load
// ============================================================

static int current_pos_ms(void) {
    if (app.play_album < 0) return 0;
    if (g_decoder) {
        // Direct position from mpg123 decoder
        if (app.is_paused) return app.play_start_offset_ms;
        return decoder_pos_ms();
    }
    // Fallback: SDL_mixer time-based estimate
    if (app.is_paused) return app.play_start_offset_ms;
    return app.play_start_offset_ms + (int)(SDL_GetTicks() - app.play_start_ticks);
}

// Save progress for all albums that have been started.
// Format: one line per album: "dirpath|track_offset|position_ms"
// The last-played album is saved first so it becomes the resume target.
static void save_progress(void) {
    save_settings();
    // Update current album's in-memory progress
    if (app.play_album >= 0) {
        Album *alb = &g_albums[app.play_album];
        alb->saved_track = app.play_track - alb->track_start;
        alb->saved_position_ms = current_pos_ms();
    }

    FILE *f = fopen(SAVE_FILE, "w");
    if (!f) return;

    // Write last-played album first (used for resume prompt)
    if (app.play_album >= 0) {
        Album *alb = &g_albums[app.play_album];
        fprintf(f, "%s|%d|%d\n", alb->dirpath,
                alb->saved_track, alb->saved_position_ms);
    }

    // Write all other albums with progress
    for (int i = 0; i < g_album_total; i++) {
        if (i == app.play_album) continue;
        Album *alb = &g_albums[i];
        if (alb->saved_track > 0 || alb->saved_position_ms > 0)
            fprintf(f, "%s|%d|%d\n", alb->dirpath,
                    alb->saved_track, alb->saved_position_ms);
    }
    fclose(f);
}

static void load_progress(void) {
    FILE *f = fopen(SAVE_FILE, "r");
    if (!f) return;

    char line[MAX_PATH_LEN + 32];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        // Parse "dirpath|track|ms"
        char *p1 = strrchr(line, '|');
        if (!p1 || p1 == line) continue;
        *p1 = '\0';
        int ms = atoi(p1 + 1);
        if (ms < 0) ms = 0;

        char *p2 = strrchr(line, '|');
        if (!p2 || p2 == line) continue;
        *p2 = '\0';
        int trk = atoi(p2 + 1);
        if (trk < 0) trk = 0;

        const char *dir = line;

        for (int i = 0; i < g_album_total; i++) {
            if (strcmp(g_albums[i].dirpath, dir) == 0) {
                // Validate track index against album
                if (trk >= g_albums[i].track_count) trk = 0;
                g_albums[i].saved_track = trk;
                g_albums[i].saved_position_ms = ms;
                log_msg("Progress loaded: album %d track %d @ %d ms", i, trk, ms);

                // First entry = last played = resume candidate
                if (first && (ms > 0 || trk > 0)) {
                    app.resume_album = i;
                    for (int a = 0; a < g_artist_total; a++) {
                        Artist *art = &g_artists[a];
                        if (i >= art->album_start &&
                            i < art->album_start + art->album_count) {
                            app.selected = a;
                            app.resume_artist = a;
                            break;
                        }
                    }
                    app.show_resume = 1;
                }
                first = 0;
                break;
            }
        }
    }
    fclose(f);
}

// ============================================================
//  Playback
// ============================================================

static void stop_playback(void) {
    decoder_close();
    if (app.music) { Mix_HaltMusic(); Mix_FreeMusic(app.music); app.music = NULL; }
    app.play_artist = -1;
    app.play_album  = -1;
    app.play_track  = -1;
    app.is_paused   = 0;
    app.play_start_offset_ms = 0;
    stay_awake_set(0);
}

static void do_play(int artist_idx, int album_idx, int track_idx, int seek_ms) {
    if (!app.audio_ok) return;
    Album *alb = &g_albums[album_idx];
    if (track_idx < alb->track_start ||
        track_idx >= alb->track_start + alb->track_count) return;

    Track *trk = &g_tracks[track_idx];
    log_msg("Play: artist=%d album=%d track=%d '%s'",
            artist_idx, album_idx, track_idx, trk->filepath);

    g_pending_seek_ms = -1;
    save_progress();
    decoder_close();
    if (app.music) { Mix_HaltMusic(); Mix_FreeMusic(app.music); app.music = NULL; }

    app.play_artist = artist_idx;
    app.play_album  = album_idx;
    app.play_track  = track_idx;
    app.is_paused   = 0;
    app.error_msg[0] = '\0';

    if (g_mpg123_lib) {
        // Use our own decoder for variable speed
        if (!decoder_open(trk->filepath, seek_ms)) {
            snprintf(app.error_msg, sizeof(app.error_msg), "Cannot load file");
            app.play_artist = -1;
            app.play_album  = -1;
            app.play_track  = -1;
            stay_awake_set(0);
            return;
        }
    } else {
        // SDL_mixer native playback (no speed control on Miyoo)
        app.music = Mix_LoadMUS(trk->filepath);
        if (!app.music) {
            log_msg("LoadMUS failed: %s", Mix_GetError());
            snprintf(app.error_msg, sizeof(app.error_msg), "Cannot load file");
            app.play_artist = -1;
            app.play_album  = -1;
            app.play_track  = -1;
            stay_awake_set(0);
            return;
        }
        Mix_VolumeMusic(app.volume);
        Mix_PlayMusic(app.music, 0);
        if (seek_ms > 1000) Mix_SetMusicPosition(seek_ms / 1000.0);
    }
    app.play_start_offset_ms = seek_ms;
    app.play_start_ticks = SDL_GetTicks();
    stay_awake_set(1);
}

static void toggle_pause(void) {
    if (app.play_album < 0) return;
    app.play_start_offset_ms = current_pos_ms();
    if (app.is_paused) {
        app.play_start_ticks = SDL_GetTicks();
        if (app.music) Mix_ResumeMusic();
        app.is_paused = 0;
        stay_awake_set(1);
    } else {
        if (app.music) Mix_PauseMusic();
        app.is_paused = 1;
        stay_awake_set(0);
    }
}

static void seek_rel(int delta_sec) {
    if (app.play_album < 0) return;
    Album *alb = &g_albums[app.play_album];
    // Accumulate rapid presses using pending position, not current playback pos
    int cur = (g_pending_seek_ms >= 0) ? g_pending_seek_ms : current_pos_ms();
    int ms = cur + delta_sec * 1000;
    if (ms < 0) {
        // Go to previous track and seek near its end
        int prev = app.play_track - 1;
        g_pending_seek_ms = -1;
        if (prev >= alb->track_start) {
            Track *prev_trk = &g_tracks[prev];
            int seek_pos = prev_trk->duration_ms + ms;  // ms is negative
            if (seek_pos < 0) seek_pos = 0;
            do_play(app.play_artist, app.play_album, prev, seek_pos);
        } else {
            // Already at first track: seek to 0
            app.play_start_offset_ms = 0;
            g_pending_seek_ms = 0;
        }
        return;
    }
    app.play_start_offset_ms = ms;  // update display immediately
    if (g_decoder) {
        // Seek immediately with our decoder
        decoder_seek(ms);
    } else {
        g_pending_seek_ms = ms;      // actual Mix_SetMusicPosition deferred to main loop
    }
}


static void play_next(void) {
    if (app.play_album < 0) return;
    Album *alb = &g_albums[app.play_album];
    int next = app.play_track + 1;
    if (next < alb->track_start + alb->track_count) {
        do_play(app.play_artist, app.play_album, next, 0);
        if (app.view == VIEW_TRACKS && app.cur_album == app.play_album)
            app.selected = next - alb->track_start;
    } else {
        log_msg("Album finished");
        alb->saved_track = 0;
        alb->saved_position_ms = 0;
        stop_playback();
    }
}

// ============================================================
//  Accessors for current view
// ============================================================

static int item_count(void) {
    if (app.view == VIEW_ARTISTS) return g_artist_total;
    if (app.view == VIEW_ALBUMS) {
        if (app.cur_artist < 0 || app.cur_artist >= g_artist_total) return 0;
        return g_artists[app.cur_artist].album_count;
    }
    if (app.cur_album < 0 || app.cur_album >= g_album_total) return 0;
    return g_albums[app.cur_album].track_count;
}

static Album *cur_album_ptr(void) {
    return &g_albums[app.cur_album];
}

// ============================================================
//  Resume screen
// ============================================================

static void render_resume(void) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x1A, 0x1A, 0x2E));

    if (app.resume_album < 0 || app.resume_album >= g_album_total ||
        app.resume_artist < 0 || app.resume_artist >= g_artist_total) {
        app.show_resume = 0;
        return;
    }
    Album *alb = &g_albums[app.resume_album];
    Artist *art = &g_artists[app.resume_artist];
    int trk_idx = alb->track_start + alb->saved_track;
    if (trk_idx < 0 || trk_idx >= g_track_total) {
        app.show_resume = 0;
        return;
    }
    Track *trk = &g_tracks[trk_idx];

    // Title
    draw_text(app.font_large, S()->continue_listening,
              rgb(0x4F, 0xC3, 0xF7), 24, 60);

    // Divider
    fill_rect(0, 110, SCREEN_W, 2,
              SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));

    // Artist
    char buf[80];
    truncate_str(art->title, buf, 50);
    draw_text(app.font_small, buf, rgb(0x88, 0x88, 0x99), 40, 140);

    // Album
    truncate_str(alb->title, buf, 50);
    draw_text(app.font_large, buf, rgb(0xEE, 0xEE, 0xEE), 40, 170);

    // Track info
    char time_str[16];
    format_time(alb->saved_position_ms, time_str, sizeof(time_str));
    snprintf(buf, sizeof(buf), "Track %d / %d",
             alb->saved_track + 1, alb->track_count);
    draw_text(app.font_small, buf, rgb(0x88, 0x88, 0x99), 40, 220);

    // Timestamp
    snprintf(buf, sizeof(buf), "> %s", time_str);
    draw_text(app.font_large, buf, rgb(0x44, 0xCC, 0x66), 40, 250);

    // Track name – prominent
    truncate_str(trk->title, buf, 45);
    draw_text(app.font_large, buf, rgb(0xDD, 0xDD, 0xEE), 40, 295);

    // Progress bar (visual hint of position in album)
    if (alb->track_count > 1) {
        int bar_w = SCREEN_W - 80;
        float progress = (float)(alb->saved_track) / (alb->track_count - 1);
        int filled = (int)(bar_w * progress);
        fill_rect(40, 340, bar_w, 8,
                  SDL_MapRGB(app.screen->format, 0x22, 0x33, 0x44));
        fill_rect(40, 340, filled, 8,
                  SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
    }

    // Buttons
    fill_rect(0, SCREEN_H - 80, SCREEN_W, 80,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));

    // A button - resume
    fill_rect(40, SCREEN_H - 65, SCREEN_W - 80, 44,
              SDL_MapRGB(app.screen->format, 0x16, 0x35, 0x50));
    fill_rect(40, SCREEN_H - 65, 4, 44,
              SDL_MapRGB(app.screen->format, 0x44, 0xCC, 0x66));
    draw_text(app.font_small, "A: Continue",
              rgb(0x44, 0xCC, 0x66), SCREEN_W / 2 - 60, SCREEN_H - 52);

    // B hint
    draw_text(app.font_small, "B: Browse library",
              rgb(0x55, 0x55, 0x66), SCREEN_W / 2 - 70, SCREEN_H - 18);

    SDL_Flip(app.screen);
}

static void handle_resume_input(SDL_Event *e) {
    if (e->type != SDL_KEYDOWN) return;

    if (g_screen_off) { screen_on(); return; }
    screen_on();

    switch (e->key.keysym.sym) {
    case SDLK_SPACE:   // A – resume playback
    case SDLK_RETURN:  // Start – also resume
    {
        if (app.resume_album < 0 || app.resume_album >= g_album_total) {
            app.show_resume = 0; break;
        }
        Album *alb = &g_albums[app.resume_album];
        int resume_ms = alb->saved_position_ms - 3000;
        if (resume_ms < 0) resume_ms = 0;
        do_play(app.resume_artist, app.resume_album,
                alb->track_start + alb->saved_track,
                resume_ms);
        // Go straight to Now Playing
        app.cur_artist = app.resume_artist;
        app.cur_album  = app.resume_album;
        app.view = VIEW_NOWPLAYING;
        app.show_resume = 0;
        break;
    }
    case SDLK_LCTRL:   // B – go to library
    case SDLK_ESCAPE:  // Menu
        app.show_resume = 0;
        break;
    default:
        break;
    }
}

// ============================================================
//  Startup animation
// ============================================================

static void draw_startup(void) {
    int cx = SCREEN_W / 2, cy = SCREEN_H / 2 - 30;

    for (int frame = 0; frame <= 40; frame++) {
        float t = (float)frame / 40;   // 0..1 over ~2 seconds

        SDL_FillRect(app.screen, NULL,
            SDL_MapRGB(app.screen->format,
                       (Uint8)(0x12*t), (Uint8)(0x12*t), (Uint8)(0x24*t)));

        // --- Open book shape ---
        if (frame > 5) {
            float bt = (frame - 5) / 25.0f;
            if (bt > 1.0f) bt = 1.0f;
            int bh = (int)(100 * bt), bw = (int)(60 * bt);
            Uint32 book_col = SDL_MapRGB(app.screen->format,
                (Uint8)(0x2A*bt), (Uint8)(0x5A*bt), (Uint8)(0x7A*bt));
            // Left page
            fill_rect(cx - bw - 2, cy - bh/2, bw, bh, book_col);
            // Right page
            fill_rect(cx + 2, cy - bh/2, bw, bh, book_col);
            // Spine
            fill_rect(cx - 2, cy - bh/2, 4, bh,
                SDL_MapRGB(app.screen->format,
                    (Uint8)(0x4F*bt), (Uint8)(0xC3*bt), (Uint8)(0xF7*bt)));
            // Page lines (left)
            for (int l = 0; l < 5 && bt > 0.3f; l++) {
                int ly = cy - bh/2 + 15 + l * 15;
                int lw = (int)((bw - 20) * ((bt - 0.3f) / 0.7f));
                fill_rect(cx - bw + 10, ly, lw, 2,
                    SDL_MapRGB(app.screen->format, 0x1A, 0x3A, 0x5A));
            }
        }

        // --- Sound waves emanating from book ---
        if (frame > 18) {
            float wt = (frame - 18) / 22.0f;
            if (wt > 1.0f) wt = 1.0f;
            Uint32 wave_col = SDL_MapRGB(app.screen->format,
                (Uint8)(0x4F*wt), (Uint8)(0xC3*wt), (Uint8)(0xF7*wt));
            for (int w = 0; w < 3; w++) {
                float wp = wt - w * 0.2f;
                if (wp <= 0) continue;
                if (wp > 1.0f) wp = 1.0f;
                int r = (int)(30 + w * 22) * (int)(wp * 10) / 10;
                int alpha_fade = (int)(wp * 255);
                if (alpha_fade < 50) continue;
                // Draw arc segments (right side of book)
                for (int a = -3; a <= 3; a++) {
                    int ax = cx + 70 + r;
                    int ay = cy + a * (r / 3);
                    fill_rect(ax, ay, 3, 4, wave_col);
                }
            }
        }

        // --- Title text ---
        if (frame > 15) {
            float tt = (frame - 15) / 15.0f;
            if (tt > 1.0f) tt = 1.0f;
            draw_text(app.font_large, "Audiobooks",
                      rgb((Uint8)(0x4F*tt), (Uint8)(0xC3*tt), (Uint8)(0xF7*tt)),
                      cx - 82, cy + 70);
        }

        // --- Subtitle ---
        if (frame > 25) {
            draw_text(app.font_small, "for Miyoo Mini+",
                      rgb(0x44, 0x66, 0x88), cx - 65, cy + 105);
        }

        // --- Loading bar ---
        if (frame > 10) {
            float lb = (frame - 10) / 30.0f;
            if (lb > 1.0f) lb = 1.0f;
            int bw2 = 300;
            fill_rect(cx - bw2/2, cy + 140, bw2, 4,
                SDL_MapRGB(app.screen->format, 0x22, 0x33, 0x44));
            fill_rect(cx - bw2/2, cy + 140, (int)(bw2 * lb), 4,
                SDL_MapRGB(app.screen->format,
                    (Uint8)(0x4F*lb), (Uint8)(0xC3*lb), (Uint8)(0xF7*lb)));
        }

        SDL_Flip(app.screen);
        SDL_Delay(50);
    }
    SDL_Delay(200);
}

// ============================================================
//  Rendering
// ============================================================

// Forward declarations for lock helpers (defined later)
static void render_lock_indicator(void);
static void render_lock_confirm(void);

// ============================================================
//  Now Playing screen
// ============================================================

static void load_cover_art(void);  // forward decl

static void render_nowplaying(void) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x12, 0x12, 0x24));

    if (app.play_artist < 0 || app.play_artist >= g_artist_total ||
        app.play_album < 0  || app.play_album >= g_album_total ||
        app.play_track < 0  || app.play_track >= g_track_total) {
        draw_text(app.font_large, S()->nothing_playing,
                  rgb(0x88, 0x88, 0x99), SCREEN_W/2 - 100, SCREEN_H/2 - 20);
        SDL_Flip(app.screen);
        return;
    }

    Artist *art = &g_artists[app.play_artist];
    Album  *alb = &g_albums[app.play_album];
    Track  *trk = &g_tracks[app.play_track];
    int track_num = app.play_track - alb->track_start + 1;

    // --- Clock (top center) ---
    {
        time_t now = time(NULL) + g_tz_offset_sec;
        struct tm *tm_now = gmtime(&now);
        char clock_buf[16];
        if (g_clock_24h) {
            snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", tm_now->tm_hour, tm_now->tm_min);
        } else {
            int h = tm_now->tm_hour % 12;
            if (h == 0) h = 12;
            snprintf(clock_buf, sizeof(clock_buf), "%d:%02d %s",
                     h, tm_now->tm_min, tm_now->tm_hour >= 12 ? "PM" : "AM");
        }
        draw_text_center(app.font_small, clock_buf, rgb(0x66, 0x77, 0x88), SCREEN_W/2, 14);
    }

    // --- Artist (small, top left) ---
    char buf[80];
    truncate_str(art->title, buf, 34);
    draw_text(app.font_small, buf, rgb(0x66, 0x77, 0x88), 20, 20);

    // --- Album ---
    truncate_str(alb->title, buf, 34);
    draw_text(app.font_small, buf, rgb(0x88, 0x99, 0xAA), 20, 46);

    // Divider (only left side, under text, before the cover)
    fill_rect(20, 80, SCREEN_W - 210, 1,
              SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));

    // --- Book cover (upper right) ---
    load_cover_art();
    {
        int cx = SCREEN_W - 170, cy = 10, cw = 140, ch = 140;
        // Background fill
        fill_rect(cx - 4, cy - 2, cw + 8, ch + 4,
                  SDL_MapRGB(app.screen->format, 0x12, 0x12, 0x24));

        if (app.cover_art) {
            // Draw actual cover image, centered in the box
            SDL_Rect dst;
            dst.x = cx + (cw - app.cover_art->w) / 2;
            dst.y = cy + (ch - app.cover_art->h) / 2;
            dst.w = app.cover_art->w;
            dst.h = app.cover_art->h;
            SDL_BlitSurface(app.cover_art, NULL, app.screen, &dst);
            // Border
            fill_rect(dst.x-2,          dst.y-2,          dst.w+4, 2,   SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(dst.x-2,          dst.y+dst.h,      dst.w+4, 2,   SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(dst.x-2,          dst.y-2,          2,  dst.h+4, SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(dst.x+dst.w,      dst.y-2,          2,  dst.h+4, SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
        } else {
            // Placeholder
            fill_rect(cx, cy, cw, ch,
                      SDL_MapRGB(app.screen->format, 0x1A, 0x28, 0x3A));
            fill_rect(cx,      cy,      cw, 2,  SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(cx,      cy+ch-2, cw, 2,  SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(cx,      cy,      2,  ch, SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(cx+cw-2, cy,      2,  ch, SDL_MapRGB(app.screen->format, 0x2A, 0x5A, 0x7A));
            fill_rect(cx+14, cy+4, 6, ch-8, SDL_MapRGB(app.screen->format, 0x33, 0x66, 0x88));
            draw_text(app.font_small, "[ cover ]",
                      rgb(0x33, 0x55, 0x77), cx + 22, cy + ch/2 - 10);
        }
    }

    // --- Track title (large, scrolling if too long) ---
    {
        int title_max_w = SCREEN_W - 190;  // leave room for cover
        int tw, th;
        TTF_SizeText(app.font_large, trk->title, &tw, &th);

        // Reset scroll state when track changes
        if (app.title_scroll_track != app.play_track) {
            app.title_scroll_track = app.play_track;
            app.title_scroll_x = 0;
            app.title_scroll_state = 0;  // waiting at start
            app.title_scroll_wait = SDL_GetTicks();
            app.title_scroll_w = tw;
            app.title_scroll_max = title_max_w;
        }

        if (tw <= title_max_w) {
            // Fits — draw directly
            draw_text(app.font_large, trk->title, rgb(0xEE, 0xEE, 0xEE), 20, 95);
        } else {
            // Scrolling logic
            Uint32 now = SDL_GetTicks();
            if (app.title_scroll_state == 0) {
                // Waiting at start for 3 seconds
                if (now - app.title_scroll_wait >= 3000) {
                    app.title_scroll_state = 1;  // start scrolling
                }
            } else if (app.title_scroll_state == 1) {
                // Scrolling: 1 pixel per frame (~30-60fps → ~30-60 px/s)
                app.title_scroll_x += 1;
                int max_scroll = tw - title_max_w;
                if (app.title_scroll_x >= max_scroll) {
                    app.title_scroll_x = max_scroll;
                    app.title_scroll_state = 2;  // waiting at end
                    app.title_scroll_wait = now;
                }
            } else if (app.title_scroll_state == 2) {
                // Waiting at end for 3 seconds
                if (now - app.title_scroll_wait >= 3000) {
                    app.title_scroll_x = 0;
                    app.title_scroll_state = 0;  // back to start
                    app.title_scroll_wait = now;
                }
            }

            // Render with clip rect
            SDL_Rect clip = { 20, 90, title_max_w, th + 10 };
            SDL_SetClipRect(app.screen, &clip);
            draw_text(app.font_large, trk->title, rgb(0xEE, 0xEE, 0xEE),
                      20 - app.title_scroll_x, 95);
            SDL_SetClipRect(app.screen, NULL);
        }
    }

    // --- Track number ---
    snprintf(buf, sizeof(buf), S()->np_track, track_num, alb->track_count);
    draw_text(app.font_small, buf, rgb(0x66, 0x77, 0x88), 20, 140);

    // --- Big time display ---
    int ms = current_pos_ms();
    int dur = trk->duration_ms;
    char time_str[16], dur_str[16];
    format_time(ms, time_str, sizeof(time_str));

    if (dur > 0) {
        format_time(dur, dur_str, sizeof(dur_str));
        snprintf(buf, sizeof(buf), "%s  %s / %s",
                 app.is_paused ? "||" : ">", time_str, dur_str);
    } else {
        snprintf(buf, sizeof(buf), "%s  %s",
                 app.is_paused ? "||" : ">", time_str);
    }
    draw_text(app.font_large, buf,
              app.is_paused ? rgb(0x88, 0x88, 0x99) : rgb(0x44, 0xCC, 0x66),
              SCREEN_W/2 - 120, 185);

    // --- Track progress bar ---
    int bar_y = 235;
    int bar_w = SCREEN_W - 60;
    fill_rect(30, bar_y, bar_w, 8,
              SDL_MapRGB(app.screen->format, 0x22, 0x33, 0x44));
    if (dur > 0) {
        float track_pct = (float)ms / dur;
        if (track_pct > 1.0f) track_pct = 1.0f;
        int filled = (int)(bar_w * track_pct);
        fill_rect(30, bar_y, filled, 8,
                  SDL_MapRGB(app.screen->format, 0x44, 0xCC, 0x66));
        fill_rect(30 + filled - 5, bar_y - 3, 10, 14,
                  SDL_MapRGB(app.screen->format, 0x44, 0xCC, 0x66));
    }
    // Track label BELOW bar
    {
        int pct = (dur > 0) ? (ms * 100 / dur) : 0;
        if (pct > 100) pct = 100;
        snprintf(buf, sizeof(buf), S()->track_pct, pct);
        draw_text(app.font_small, buf, rgb(0x44, 0xCC, 0x66), 30, bar_y + 12);
        // Speed indicator next to track label
        if (app.speed_idx != SPEED_DEFAULT) {
            snprintf(buf, sizeof(buf), "Speed: %s", speed_labels[app.speed_idx]);
            draw_text(app.font_small, buf, rgb(0xF0, 0xA0, 0x30), SCREEN_W - 180, bar_y + 12);
        }
    }

    // --- Audiobook progress bar ---
    int bar2_y = bar_y + 40;
    fill_rect(30, bar2_y, bar_w, 6,
              SDL_MapRGB(app.screen->format, 0x22, 0x33, 0x44));
    if (alb->track_count > 1) {
        float album_pct = (float)(track_num - 1) / (alb->track_count - 1);
        int filled2 = (int)(bar_w * album_pct);
        fill_rect(30, bar2_y, filled2, 6,
                  SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
    }
    // Audiobook label BELOW bar
    {
        int pct = alb->track_count > 1
                  ? (track_num - 1) * 100 / (alb->track_count - 1) : 0;
        snprintf(buf, sizeof(buf), S()->audiobook_pct, pct);
        draw_text(app.font_small, buf, rgb(0x4F, 0xC3, 0xF7), 30, bar2_y + 10);
    }

    // --- Controls hint ---
    fill_rect(0, SCREEN_H - 90, SCREEN_W, 90,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));

    if (app.locked) {
        // When locked, only show unlock instruction centered
        draw_text_center(app.font_small, S()->unlock_hold,
                         rgb(0x55, 0x55, 0x66), SCREEN_W/2, SCREEN_H - 32);
    } else {
        // Row 1: main controls
        draw_text(app.font_small, "L1:-15s",
                  rgb(0x4F, 0xC3, 0xF7), 20, SCREEN_H - 84);
        draw_text(app.font_small, app.is_paused ? S()->np_play : S()->np_pause,
                  rgb(0x44, 0xCC, 0x66), SCREEN_W/2 - 30, SCREEN_H - 84);
        draw_text(app.font_small, "R1:+15s",
                  rgb(0x4F, 0xC3, 0xF7), SCREEN_W - 100, SCREEN_H - 84);

        // Row 2: track nav + speed
        draw_text(app.font_small, S()->np_prev_next,
                  rgb(0x55, 0x55, 0x66), 20, SCREEN_H - 58);
        draw_text(app.font_small, S()->np_speed,
                  rgb(0x55, 0x55, 0x66), SCREEN_W - 140, SCREEN_H - 58);

        // Row 3: navigation + lock
        draw_text(app.font_small, S()->np_browse_back,
                  rgb(0x55, 0x55, 0x66), 20, SCREEN_H - 32);
        draw_text_right(app.font_small, S()->lock_hint,
                        rgb(0x44, 0x55, 0x66), SCREEN_W - 10, SCREEN_H - 32);
    }

    if (app.locked) render_lock_indicator();
    if (app.lock_confirm) render_lock_confirm();
    SDL_Flip(app.screen);
}

static void handle_nowplaying(SDL_Event *e) {
    if (e->type != SDL_KEYDOWN) return;
    if (g_screen_off) { screen_on(); return; }
    screen_on();

    switch (e->key.keysym.sym) {
    case SDLK_SPACE:   // A – pause/resume
        toggle_pause();
        break;

    case SDLK_RETURN:  // Start – back to browse view
        if (app.saved_view != VIEW_NOWPLAYING) {
            app.view = app.saved_view;
            app.selected = app.saved_selected;
        } else {
            app.view = VIEW_TRACKS;
            if (app.play_album >= 0) {
                app.cur_artist = app.play_artist;
                app.cur_album = app.play_album;
                Album *alb = &g_albums[app.play_album];
                app.selected = app.play_track - alb->track_start;
            }
        }
        break;

    case SDLK_LCTRL:   // B – back to track list
        app.view = VIEW_TRACKS;
        if (app.play_album >= 0) {
            app.cur_artist = app.play_artist;
            app.cur_album = app.play_album;
            Album *alb = &g_albums[app.play_album];
            app.selected = app.play_track - alb->track_start;
        }
        break;

    case SDLK_UP:      // Speed up
        if (app.speed_idx < SPEED_COUNT - 1) app.speed_idx++;
        break;

    case SDLK_DOWN:    // Speed down
        if (app.speed_idx > 0) app.speed_idx--;
        break;

    case SDLK_e:       // L1 – rewind 15s
        seek_rel(-15);
        break;

    case SDLK_t:       // R1 – forward 15s
        seek_rel(+15);
        break;

    case SDLK_LEFT:    // prev track
        if (app.play_album >= 0) {
            Album *alb = &g_albums[app.play_album];
            int prev = app.play_track - 1;
            if (prev >= alb->track_start)
                do_play(app.play_artist, app.play_album, prev, 0);
        }
        break;

    case SDLK_RIGHT:   // next track
        if (app.play_album >= 0) {
            Album *alb = &g_albums[app.play_album];
            int next = app.play_track + 1;
            if (next < alb->track_start + alb->track_count)
                do_play(app.play_artist, app.play_album, next, 0);
        }
        break;

    // L2/R2 reserved for lock combo – physical volume buttons handle audio

    default: break;
    }
}

// ============================================================
//  Quit confirmation
// ============================================================

static void render_quit_confirm(void) {
    // Semi-transparent overlay on top of current screen
    fill_rect(0, 0, SCREEN_W, SCREEN_H,
              SDL_MapRGB(app.screen->format, 0x10, 0x10, 0x1C));

    // Dialog box
    int bx = 80, by = 160, bw = SCREEN_W - 160, bh = 160;
    fill_rect(bx, by, bw, bh,
              SDL_MapRGB(app.screen->format, 0x1A, 0x2A, 0x3E));
    // Border
    fill_rect(bx, by, bw, 2,
              SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
    fill_rect(bx, by + bh - 2, bw, 2,
              SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));

    draw_text(app.font_large, S()->quit_title,
              rgb(0xEE, 0xEE, 0xEE), bx + bw/2 - 120, by + 30);

    draw_text(app.font_small, S()->quit_yes,
              rgb(0xEF, 0x44, 0x44), bx + bw/2 - 80, by + 85);
    draw_text(app.font_small, S()->quit_no,
              rgb(0x44, 0xCC, 0x66), bx + bw/2 - 80, by + 115);

    SDL_Flip(app.screen);
}

static void handle_quit_confirm(SDL_Event *e) {
    if (e->type != SDL_KEYDOWN) return;
    if (g_screen_off) { screen_on(); return; }
    screen_on();

    switch (e->key.keysym.sym) {
    case SDLK_SPACE:   // A – yes, quit
        save_progress();
        app.running = 0;
        break;
    case SDLK_LCTRL:   // B – no, go back (stays paused)
    case SDLK_ESCAPE:  // Menu – also cancel
        app.show_quit_confirm = 0;
        break;
    default:
        break;
    }
}

// ============================================================
//  Options screen
// ============================================================
//  Cover art download
// ============================================================

// URL-encode a string (spaces → +, skip most special chars)
static void url_encode(const char *src, char *dst, int dst_size) {
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 4; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            dst[j++] = c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            snprintf(dst + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    dst[j] = '\0';
}

// Extract artist name from album dirpath (parent directory name)
static void get_artist_from_path(const char *dirpath, char *artist, int size) {
    // dirpath = "/mnt/SDCARD/Audiobooks/ArtistName/AlbumName"
    // We want "ArtistName"
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, dirpath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    // Remove trailing slash
    int len = strlen(tmp);
    if (len > 0 && tmp[len-1] == '/') tmp[--len] = '\0';

    // Find last two slashes
    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) { strncpy(artist, "Unknown", size); return; }
    *last_slash = '\0';
    char *second_slash = strrchr(tmp, '/');
    if (second_slash)
        strncpy(artist, second_slash + 1, size);
    else
        strncpy(artist, tmp, size);
    artist[size - 1] = '\0';
}

// Search OpenLibrary for a cover and download it
// Returns 1 if cover was found and downloaded, 0 otherwise
static int search_and_download_cover(const char *artist, const char *album,
                                      const char *cover_path) {
    char query[256], encoded[512];
    snprintf(query, sizeof(query), "%s %s", artist, album);
    url_encode(query, encoded, sizeof(encoded));

    log_msg("Cover search: artist='%s' album='%s'", artist, album);

    // Search OpenLibrary
    // Must use env -i to clear LD_PRELOAD (breaks curl) and set clean LD_LIBRARY_PATH
    #define CURL_ENV "env -i " \
        "LD_LIBRARY_PATH=/lib:/config/lib:/mnt/SDCARD/miyoo/lib" \
        ":/mnt/SDCARD/.tmp_update/lib:/mnt/SDCARD/.tmp_update/lib/parasyte" \
        ":/mnt/SDCARD/.tmp_update/lib/samba " \
        "HOME=/root"
    char cmd[1024], response[8192];
    const char *tmp_json = "/tmp/cover_search.json";
    snprintf(cmd, sizeof(cmd),
        CURL_ENV " " CURL_PATH " --insecure --silent --max-time 10 "
        "-o %s "
        "'https://openlibrary.org/search.json?q=%s&limit=1'",
        tmp_json, encoded);

    int sys_ret = system(cmd);
    log_msg("  curl ret=%d", sys_ret);

    // Read response from temp file
    int total_read = 0;
    FILE *rf = fopen(tmp_json, "r");
    if (rf) {
        total_read = fread(response, 1, sizeof(response) - 1, rf);
        fclose(rf);
        remove(tmp_json);
    }
    response[total_read] = '\0';

    log_msg("  response: %d bytes", total_read);
    if (total_read < 200 && total_read > 0)
        log_msg("  body: %s", response);

    // Find cover_i in JSON response
    char *ci = strstr(response, "\"cover_i\":");
    if (!ci) { log_msg("  no cover_i found"); return 0; }
    int cover_id = 0;
    if (sscanf(ci, "\"cover_i\":%d", &cover_id) != 1 || cover_id <= 0) {
        log_msg("  cover_i parse failed");
        return 0;
    }

    log_msg("  cover_id: %d → downloading...", cover_id);

    // Download cover image (Medium size = ~180px)
    snprintf(cmd, sizeof(cmd),
        CURL_ENV " " CURL_PATH " --insecure --silent --location --max-time 10 "
        "'https://covers.openlibrary.org/b/id/%d-M.jpg' "
        "-o '%s'",
        cover_id, cover_path);
    int ret = system(cmd);
    log_msg("  download ret=%d", ret);
    if (ret != 0) return 0;

    // Verify it's a valid file (at least 1KB)
    struct stat st;
    if (stat(cover_path, &st) != 0 || st.st_size < 1024) {
        log_msg("  file too small or missing (%ld bytes)", (long)st.st_size);
        remove(cover_path);
        return 0;
    }
    log_msg("  OK! saved %ld bytes to %s", (long)st.st_size, cover_path);
    return 1;
}

// Render cover download progress screen
static void render_cover_progress(int checked, int total, const char *current_name,
                                   int found) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x1A, 0x1A, 0x2E));

    draw_text(app.font_large, S()->download_covers,
              rgb(0x4F, 0xC3, 0xF7), 24, 14);
    fill_rect(0, 54, SCREEN_W, 2,
              SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));

    // Progress bar
    int bar_y = 200, bar_h = 16;
    fill_rect(40, bar_y, SCREEN_W - 80, bar_h,
              SDL_MapRGB(app.screen->format, 0x2A, 0x2A, 0x3E));
    if (total > 0) {
        int filled = (SCREEN_W - 80) * checked / total;
        fill_rect(40, bar_y, filled, bar_h,
                  SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
    }

    // Progress text
    char buf[80];
    snprintf(buf, sizeof(buf), "%d / %d", checked, total);
    draw_text(app.font_large, buf, rgb(0xEE, 0xEE, 0xEE), SCREEN_W/2 - 40, 140);

    // Current album name
    char status[120];
    char short_name[40];
    truncate_str(current_name, short_name, 36);
    snprintf(status, sizeof(status), S()->cover_searching, short_name);
    draw_text(app.font_small, status, rgb(0x88, 0x88, 0x99), 40, 240);

    // Found so far
    snprintf(buf, sizeof(buf), S()->cover_result, found, checked);
    draw_text(app.font_small, buf, rgb(0x44, 0xCC, 0x66), 40, 270);

    // Cancel hint
    fill_rect(0, SCREEN_H - 36, SCREEN_W, 36,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));
    draw_text(app.font_small, S()->press_b_cancel,
              rgb(0x55, 0x55, 0x66), 10, SCREEN_H - 26);

    SDL_Flip(app.screen);
}

// Render cover download result screen
static void render_cover_result(int found, int total) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x1A, 0x1A, 0x2E));

    draw_text(app.font_large, S()->cover_done,
              rgb(0x4F, 0xC3, 0xF7), 24, 14);
    fill_rect(0, 54, SCREEN_W, 2,
              SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));

    char buf[80];
    snprintf(buf, sizeof(buf), S()->cover_result, found, total);
    draw_text(app.font_large, buf,
              found > 0 ? rgb(0x44, 0xCC, 0x66) : rgb(0xEF, 0x44, 0x44),
              SCREEN_W/2 - 120, SCREEN_H/2 - 20);

    fill_rect(0, SCREEN_H - 36, SCREEN_W, 36,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));
    draw_text(app.font_small, "B: OK",
              rgb(0x55, 0x55, 0x66), 10, SCREEN_H - 26);

    SDL_Flip(app.screen);
}

// Main cover download routine (blocking with cancel support)
static void do_download_covers(void) {
    int found = 0, checked = 0;
    int total = g_album_total;
    int cancelled = 0;

    for (int i = 0; i < g_album_total && !cancelled; i++) {
        Album *alb = &g_albums[i];

        // Check if cover.jpg already exists
        char cover_path[MAX_PATH_LEN + 16];
        snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", alb->dirpath);
        if (access(cover_path, F_OK) == 0) {
            found++;
            checked++;
            render_cover_progress(checked, total, alb->title, found);
            continue;
        }

        // Render progress
        render_cover_progress(checked, total, alb->title, found);

        // Check for cancel (B button)
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_LCTRL) {
                cancelled = 1;
                break;
            }
        }
        if (cancelled) break;

        // Get artist name from directory path
        char artist[256];
        get_artist_from_path(alb->dirpath, artist, sizeof(artist));

        // Search and download
        if (search_and_download_cover(artist, alb->title, cover_path))
            found++;

        checked++;
        render_cover_progress(checked, total, alb->title, found);
    }

    // Show result screen, wait for B
    render_cover_result(found, total);
    int done = 0;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN &&
                (e.key.keysym.sym == SDLK_LCTRL ||
                 e.key.keysym.sym == SDLK_SPACE)) {
                done = 1;
            }
        }
        SDL_Delay(30);
    }
}

// Load cover art for the currently playing album
static void load_cover_art(void) {
    if (app.play_album < 0) {
        if (app.cover_art) { SDL_FreeSurface(app.cover_art); app.cover_art = NULL; }
        return;
    }
    // Already cached for this album?
    if (app.cover_art && app.cover_album_id == app.play_album)
        return;

    // Free old cover
    if (app.cover_art) { SDL_FreeSurface(app.cover_art); app.cover_art = NULL; }
    app.cover_album_id = app.play_album;

    Album *alb = &g_albums[app.play_album];
    char cover_path[MAX_PATH_LEN + 16];
    snprintf(cover_path, sizeof(cover_path), "%s/cover.jpg", alb->dirpath);

    SDL_Surface *raw = IMG_Load(cover_path);
    if (!raw) {
        log_msg("Cover load failed: %s → %s", cover_path, IMG_GetError());
        return;  // cover_album_id is set, won't retry
    }
    log_msg("  raw: %dx%d, bpp=%d", raw->w, raw->h, raw->format->BitsPerPixel);

    if (raw->w <= 0 || raw->h <= 0) {
        SDL_FreeSurface(raw);
        return;
    }

    // Scale to fit ~140px height (keeping aspect ratio)
    int target_h = 140;
    int target_w = raw->w * target_h / raw->h;
    if (target_w < 10) target_w = 10;

    // Create scaled surface (simple nearest-neighbor)
    SDL_Surface *scaled = SDL_CreateRGBSurface(SDL_SWSURFACE, target_w, target_h,
                                                app.screen->format->BitsPerPixel,
                                                app.screen->format->Rmask,
                                                app.screen->format->Gmask,
                                                app.screen->format->Bmask,
                                                app.screen->format->Amask);
    if (scaled) {
        for (int y = 0; y < target_h; y++) {
            int src_y = y * raw->h / target_h;
            for (int x = 0; x < target_w; x++) {
                int src_x = x * raw->w / target_w;
                // Get pixel from source (handle different bpp safely)
                Uint8 *src_px = (Uint8 *)raw->pixels + src_y * raw->pitch +
                                src_x * raw->format->BytesPerPixel;
                Uint8 *dst_px = (Uint8 *)scaled->pixels + y * scaled->pitch +
                                x * scaled->format->BytesPerPixel;
                Uint32 src_pixel = 0;
                memcpy(&src_pixel, src_px, raw->format->BytesPerPixel);
                Uint8 r, g, b;
                SDL_GetRGB(src_pixel, raw->format, &r, &g, &b);
                Uint32 dst_pixel = SDL_MapRGB(scaled->format, r, g, b);
                memcpy(dst_px, &dst_pixel, scaled->format->BytesPerPixel);
            }
        }
        app.cover_art = scaled;
        log_msg("  scaled to %dx%d OK", target_w, target_h);
    } else {
        log_msg("  CreateRGBSurface failed");
    }
    SDL_FreeSurface(raw);
}

// ============================================================
//  About screen
// ============================================================

static int g_about_scroll = 0;

static void render_about(void) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x12, 0x12, 0x24));

    int y = 20 - g_about_scroll;
    SDL_Color h1 = rgb(0x4F, 0xC3, 0xF7);
    SDL_Color h2 = rgb(0x44, 0xCC, 0x66);
    SDL_Color tx = rgb(0xCC, 0xCC, 0xDD);
    SDL_Color dm = rgb(0x77, 0x77, 0x88);

    draw_text(app.font_large, "MiyooAudiobook v1.0.1", h1, 24, y);
    y += 34;
    draw_text(app.font_small, "Audiobook player for Miyoo Mini+", dm, 24, y);
    y += 30;

    // --- Controls ---
    fill_rect(24, y, SCREEN_W - 48, 1, SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));
    y += 10;
    draw_text(app.font_small, "Controls", h2, 24, y); y += 24;
    draw_text(app.font_small, "A ............ Play / Pause", tx, 34, y); y += 20;
    draw_text(app.font_small, "L1 / R1 ...... Seek -/+ 15s", tx, 34, y); y += 20;
    draw_text(app.font_small, "Left / Right . Previous / Next track", tx, 34, y); y += 20;
    draw_text(app.font_small, "Up / Down .... Playback speed", tx, 34, y); y += 20;
    draw_text(app.font_small, "Start ........ Browse / Now Playing", tx, 34, y); y += 20;
    draw_text(app.font_small, "Select ....... Settings", tx, 34, y); y += 20;
    draw_text(app.font_small, "Hold L2+R2 ... Lock screen", tx, 34, y); y += 28;

    // --- Folder structure ---
    fill_rect(24, y, SCREEN_W - 48, 1, SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));
    y += 10;
    draw_text(app.font_small, "Folder structure", h2, 24, y); y += 24;
    draw_text(app.font_small, "/Audiobooks/", tx, 34, y); y += 20;
    draw_text(app.font_small, "  Artist Name/", dm, 34, y); y += 20;
    draw_text(app.font_small, "    Album Name/", dm, 34, y); y += 20;
    draw_text(app.font_small, "      001_track.mp3", dm, 34, y); y += 20;
    draw_text(app.font_small, "      002_track.mp3", dm, 34, y); y += 20;
    draw_text(app.font_small, "      cover.jpg", dm, 34, y); y += 20;
    draw_text(app.font_small, "Tracks sorted alphabetically.", tx, 34, y); y += 20;
    draw_text(app.font_small, "Prefix filenames with numbers.", tx, 34, y); y += 28;

    // --- Features ---
    fill_rect(24, y, SCREEN_W - 48, 1, SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));
    y += 10;
    draw_text(app.font_small, "Features", h2, 24, y); y += 24;
    draw_text(app.font_small, "Auto-saves progress every 10 seconds", tx, 34, y); y += 20;
    draw_text(app.font_small, "Variable speed: 0.5x - 2.0x", tx, 34, y); y += 20;
    draw_text(app.font_small, "Sleep timer with auto-pause", tx, 34, y); y += 20;
    draw_text(app.font_small, "Screen lock to prevent accidental input", tx, 34, y); y += 20;
    draw_text(app.font_small, "Cover art from cover.jpg in album folder", tx, 34, y); y += 20;
    draw_text(app.font_small, "Multi-disc albums via subfolders", tx, 34, y); y += 28;

    // --- License ---
    fill_rect(24, y, SCREEN_W - 48, 1, SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));
    y += 10;
    draw_text(app.font_small, "License", h2, 24, y); y += 24;
    draw_text(app.font_small, "MIT License - Open Source", tx, 34, y); y += 20;
    draw_text(app.font_small, "github.com/smonbon/MiyooAudiobook", tx, 34, y); y += 28;

    draw_text(app.font_small, "Built with SDL 1.2, mpg123, WSOLA", dm, 34, y); y += 30;

    // Footer
    fill_rect(0, SCREEN_H - 36, SCREEN_W, 36,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));
    draw_text(app.font_small, "Up/Dn:Scroll  B:Back",
              rgb(0x55, 0x55, 0x66), 10, SCREEN_H - 26);

    SDL_Flip(app.screen);
}

static void handle_about(SDL_Event *e) {
    if (e->type != SDL_KEYDOWN) return;
    switch (e->key.keysym.sym) {
    case SDLK_UP:
        if (g_about_scroll > 0) g_about_scroll -= 20;
        break;
    case SDLK_DOWN:
        g_about_scroll += 20;
        if (g_about_scroll > 300) g_about_scroll = 300;
        break;
    case SDLK_LCTRL:   // B – back
    case SDLK_ESCAPE:
        g_show_about = 0;
        g_about_scroll = 0;
        break;
    default:
        break;
    }
}

// ============================================================

static void render_options(void) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x1A, 0x1A, 0x2E));

    draw_text(app.font_large, S()->settings,
              rgb(0x4F, 0xC3, 0xF7), 24, 14);
    fill_rect(0, 54, SCREEN_W, 2,
              SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));

    // Show reset confirmation dialog
    if (g_reset_confirm) {
        int bx = 80, by = 160, bw = SCREEN_W - 160, bh = 140;
        fill_rect(bx, by, bw, bh,
                  SDL_MapRGB(app.screen->format, 0x28, 0x10, 0x10));
        fill_rect(bx, by,     bw, 2, SDL_MapRGB(app.screen->format, 0xEF, 0x44, 0x44));
        fill_rect(bx, by+bh-2,bw, 2, SDL_MapRGB(app.screen->format, 0xEF, 0x44, 0x44));
        draw_text(app.font_small, S()->reset_confirm_q,
                  rgb(0xEE, 0xEE, 0xEE), bx + 20, by + 50);
        SDL_Flip(app.screen);
        return;
    }

    // Show reset flash if recently triggered
    if (SDL_GetTicks() < g_reset_flash_until) {
        int fy = SCREEN_H/2 - 20;
        fill_rect(60, fy, SCREEN_W - 120, 44,
                  SDL_MapRGB(app.screen->format, 0x16, 0x40, 0x20));
        draw_text(app.font_large, S()->reset_done,
                  rgb(0x44, 0xCC, 0x66), 80, fy + 8);
        SDL_Flip(app.screen);
        return;
    }

    // Build option labels (6 options, compact row height)
    const char *labels[OPTION_COUNT];
    labels[0] = S()->screen_timeout;
    labels[1] = S()->sleep_timer;
    labels[2] = S()->language;
    labels[3] = S()->source_path;
    labels[4] = S()->clock_format;
    labels[5] = S()->download_covers;
    labels[6] = S()->reset_progress;
    labels[7] = S()->about;

    int row_h = 46;
    for (int i = 0; i < OPTION_COUNT; i++) {
        int y = 62 + i * row_h;
        if (i == g_option_selected) {
            fill_rect(4, y, SCREEN_W - 8, row_h - 2,
                      SDL_MapRGB(app.screen->format, 0x16, 0x35, 0x50));
            fill_rect(0, y, 4, row_h - 2,
                      SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
        }
        draw_text(app.font_small, labels[i],
                  i == g_option_selected ? rgb(0xEE, 0xEE, 0xEE) : rgb(0x88, 0x88, 0x99),
                  30, y + 4);

        if (i == 5 || i == 6 || i == 7) {
            // Action buttons (covers, reset, about): show [ A ] hint
            SDL_Color ac = (i == 6)
                ? (i == g_option_selected ? rgb(0xEF, 0x44, 0x44) : rgb(0x66, 0x33, 0x33))
                : (i == g_option_selected ? rgb(0x4F, 0xC3, 0xF7) : rgb(0x33, 0x55, 0x66));
            draw_text(app.font_small, "[ A ]", ac, 30, y + 24);
        } else {
            char val_buf[80];
            const char *val = "";
            if (i == 0) val = timeout_labels[g_timeout_idx];
            else if (i == 1) val = SLEEP_LABEL(g_sleep_idx);
            else if (i == 2) val = lang_labels[g_lang_idx];
            else if (i == 3) {
                snprintf(val_buf, sizeof(val_buf), "< %s >", path_options[g_path_idx]);
                draw_text(app.font_small, val_buf,
                          rgb(0x4F, 0xC3, 0xF7), 30, y + 24);
                continue;
            } else if (i == 4) {
                val = g_clock_24h ? "24h" : "12h AM/PM";
            }
            snprintf(val_buf, sizeof(val_buf), "< %s >", val);
            draw_text(app.font_small, val_buf, rgb(0x4F, 0xC3, 0xF7), 30, y + 24);
        }
    }

    // Sleep timer countdown
    if (app.sleep_timer_min > 0) {
        int elapsed = (int)(SDL_GetTicks() - app.sleep_timer_start) / 1000;
        int remaining = app.sleep_timer_min * 60 - elapsed;
        if (remaining < 0) remaining = 0;
        char buf[40];
        snprintf(buf, sizeof(buf), S()->sleep_in, remaining / 60, remaining % 60);
        draw_text(app.font_small, buf, rgb(0xF0, 0xA0, 0x30), SCREEN_W - 200, 14);
    }

    // Footer
    fill_rect(0, SCREEN_H - 36, SCREEN_W, 36,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));
    if (app.locked)
        draw_text_center(app.font_small, S()->unlock_hold,
                         rgb(0x55, 0x55, 0x66), SCREEN_W/2, SCREEN_H - 26);
    else
        draw_text(app.font_small, S()->opts_hint_action,
                  rgb(0x55, 0x55, 0x66), 10, SCREEN_H - 26);

    if (app.locked) render_lock_indicator();

    SDL_Flip(app.screen);
}

static void rescan_library(void) {
    g_artist_total = 0;
    g_album_total = 0;
    g_track_total = 0;
    app.cur_artist = 0;
    app.cur_album = 0;
    app.selected = 0;
    app.scroll_offset = 0;
    app.view = VIEW_ARTISTS;
    app.play_album = -1;
    app.play_track = -1;
    app.show_resume = 0;
    scan_library();
    load_progress();
}

static int g_path_idx_on_enter = 0;  // snapshot when opening settings

static void handle_options(SDL_Event *e) {
    if (e->type != SDL_KEYDOWN) return;
    if (g_screen_off) { screen_on(); return; }
    screen_on();

    // Dismiss reset flash on any keypress → close options entirely
    if (SDL_GetTicks() < g_reset_flash_until) {
        g_reset_flash_until = 0;
        g_show_options = 0;
        return;
    }

    // Handle reset confirmation sub-dialog
    if (g_reset_confirm) {
        if (e->key.keysym.sym == SDLK_SPACE) {  // A = yes
            do_reset_progress();
            g_reset_confirm = 0;
        } else if (e->key.keysym.sym == SDLK_LCTRL) {  // B = no
            g_reset_confirm = 0;
        }
        return;
    }

    switch (e->key.keysym.sym) {
    case SDLK_UP:
        if (g_option_selected > 0) g_option_selected--;
        break;
    case SDLK_DOWN:
        if (g_option_selected < OPTION_COUNT - 1) g_option_selected++;
        break;
    case SDLK_LEFT:
        if (g_option_selected == 0)
            g_timeout_idx = (g_timeout_idx > 0) ? g_timeout_idx - 1 : TIMEOUT_COUNT - 1;
        else if (g_option_selected == 1)
            g_sleep_idx = (g_sleep_idx > 0) ? g_sleep_idx - 1 : SLEEP_COUNT - 1;
        else if (g_option_selected == 2)
            g_lang_idx = (g_lang_idx > 0) ? g_lang_idx - 1 : LANG_COUNT - 1;
        else if (g_option_selected == 3) {
            g_path_idx = (g_path_idx > 0) ? g_path_idx - 1 : PATH_COUNT - 1;
            rescan_library();
        }
        else if (g_option_selected == 4)
            g_clock_24h = !g_clock_24h;
        break;
    case SDLK_SPACE:  // A – action or cycle right
        if (g_option_selected == 5) {
            do_download_covers();  // blocking cover download
            break;
        }
        if (g_option_selected == 6) {
            g_reset_confirm = 1;  // show confirm dialog
            break;
        }
        if (g_option_selected == 7) {
            g_show_about = 1;
            break;
        }
        // fall through to RIGHT for other options
    case SDLK_RIGHT:
        if (g_option_selected == 0)
            g_timeout_idx = (g_timeout_idx < TIMEOUT_COUNT - 1) ? g_timeout_idx + 1 : 0;
        else if (g_option_selected == 1)
            g_sleep_idx = (g_sleep_idx < SLEEP_COUNT - 1) ? g_sleep_idx + 1 : 0;
        else if (g_option_selected == 2)
            g_lang_idx = (g_lang_idx < LANG_COUNT - 1) ? g_lang_idx + 1 : 0;
        else if (g_option_selected == 3) {
            g_path_idx = (g_path_idx < PATH_COUNT - 1) ? g_path_idx + 1 : 0;
            rescan_library();
        }
        else if (g_option_selected == 4)
            g_clock_24h = !g_clock_24h;
        break;
    case SDLK_LCTRL:   // B – back
        // Apply sleep timer
        if (sleep_values[g_sleep_idx] > 0) {
            app.sleep_timer_min = sleep_values[g_sleep_idx];
            app.sleep_timer_start = SDL_GetTicks();
        } else {
            app.sleep_timer_min = 0;
        }
        save_settings();
        g_show_options = 0;
        break;
    default:
        break;
    }
}

// ============================================================
//  Lock screen
// ============================================================

// Draw a small lock icon at top-right when locked
static void render_lock_indicator(void) {
    Uint32 col = SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7);
    Uint32 bg  = SDL_MapRGB(app.screen->format, 0x1A, 0x1A, 0x2E);
    int lx = SCREEN_W - 34, ly = 4;

    // Shackle (U-shape arc at top)
    fill_rect(lx + 6,  ly,      12, 3, col);   // top bar
    fill_rect(lx + 4,  ly + 2,   4, 10, col);  // left arm
    fill_rect(lx + 16, ly + 2,   4, 10, col);  // right arm
    fill_rect(lx + 8,  ly + 3,   8, 3,  bg);   // shackle inner gap

    // Body (rounded rectangle)
    fill_rect(lx + 1,  ly + 11, 22, 16, col);  // main body
    fill_rect(lx,      ly + 13, 24, 12, col);  // wider middle

    // Keyhole (circle + line)
    fill_rect(lx + 9,  ly + 16,  6, 6, bg);    // keyhole circle
    fill_rect(lx + 10, ly + 21,  4, 3, bg);    // keyhole slot
}

// Show lock/unlock confirm dialog
static void render_lock_confirm(void) {
    // Full overlay
    fill_rect(0, 0, SCREEN_W, SCREEN_H,
              SDL_MapRGB(app.screen->format, 0x10, 0x10, 0x1C));

    int bx = 80, by = 150, bw = SCREEN_W - 160, bh = 180;
    fill_rect(bx, by, bw, bh,
              SDL_MapRGB(app.screen->format, 0x1A, 0x2A, 0x3E));
    fill_rect(bx, by,     bw, 2,
              SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
    fill_rect(bx, by+bh-2,bw, 2,
              SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));

    int lx = bx + 24;  // left-align everything with padding
    if (app.locked) {
        draw_text(app.font_large, S()->unlock_screen_q,
                  rgb(0xEE, 0xEE, 0xEE), lx, by + 25);
        draw_text(app.font_small, S()->unlock_confirm,
                  rgb(0x44, 0xCC, 0x66), lx, by + 85);
        draw_text(app.font_small, S()->unlock_cancel,
                  rgb(0x88, 0x88, 0x99), lx, by + 115);
    } else {
        draw_text(app.font_large, S()->lock_screen_q,
                  rgb(0xEE, 0xEE, 0xEE), lx, by + 25);
        draw_text(app.font_small, S()->lock_confirm,
                  rgb(0x4F, 0xC3, 0xF7), lx, by + 85);
        draw_text(app.font_small, S()->lock_cancel,
                  rgb(0x88, 0x88, 0x99), lx, by + 115);
        draw_text(app.font_small, S()->unlock_to_unlock,
                  rgb(0x55, 0x55, 0x66), lx, by + 148);
    }

    SDL_Flip(app.screen);
}

// ============================================================
//  List rendering
// ============================================================

static void render(void) {
    SDL_FillRect(app.screen, NULL,
                 SDL_MapRGB(app.screen->format, 0x1A, 0x1A, 0x2E));

    // Header
    const char *hdr_src = "Audiobooks";
    if (app.view == VIEW_ALBUMS && app.cur_artist >= 0 && app.cur_artist < g_artist_total)
        hdr_src = g_artists[app.cur_artist].title;
    else if (app.view == VIEW_TRACKS && app.cur_album >= 0 && app.cur_album < g_album_total)
        hdr_src = g_albums[app.cur_album].title;
    char hdr[42]; truncate_str(hdr_src, hdr, 38);
    draw_text(app.font_large, hdr, rgb(0x4F, 0xC3, 0xF7), 24, 14);

    int count = item_count();

    // Counter
    if (count > 0) {
        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d / %d", app.selected + 1, count);
        draw_text(app.font_small, cnt, rgb(0x66, 0x77, 0x88), SCREEN_W - 100, 20);
    }

    // Now-playing
    if (app.play_album >= 0) {
        Album *pa = &g_albums[app.play_album];
        char ts[16]; format_time(current_pos_ms(), ts, sizeof(ts));
        int toff = app.play_track - pa->track_start + 1;
        char np[80];
        snprintf(np, sizeof(np), "%s %s  %d/%d",
                 app.is_paused ? "||" : ">", ts, toff, pa->track_count);
        draw_text(app.font_small, np, rgb(0x44, 0xCC, 0x66), SCREEN_W - 300, 20);
    }

    fill_rect(0, 54, SCREEN_W, 2,
              SDL_MapRGB(app.screen->format, 0x2A, 0x4A, 0x6A));

    // List
    int top = 62, row_h = 50, vis = app.visible_rows;

    if (count == 0) {
        char empty_hint[80];
        snprintf(empty_hint, sizeof(empty_hint), "No files in %s", path_options[g_path_idx]);
        draw_text(app.font_small, empty_hint, rgb(0x88, 0x88, 0x99), 24, top + 20);
    }

    // Adjust scroll_offset only when selection goes out of visible range
    if (app.selected < app.scroll_offset)
        app.scroll_offset = app.selected;
    else if (app.selected >= app.scroll_offset + vis)
        app.scroll_offset = app.selected - vis + 1;
    if (app.scroll_offset < 0) app.scroll_offset = 0;
    int scroll = app.scroll_offset;
    if (scroll > 0)
        draw_text(app.font_small, "^", rgb(0x4F, 0xC3, 0xF7), SCREEN_W-24, top);

    for (int i = 0; i < vis && (scroll+i) < count; i++) {
        int idx = scroll + i;
        int y = top + i * row_h;

        if (idx == app.selected) {
            fill_rect(4, y+1, SCREEN_W-8, row_h-2,
                      SDL_MapRGB(app.screen->format, 0x16, 0x35, 0x50));
            fill_rect(0, y+1, 4, row_h-2,
                      SDL_MapRGB(app.screen->format, 0x4F, 0xC3, 0xF7));
        }

        char label[80];
        int is_playing = 0;

        if (app.view == VIEW_ARTISTS) {
            Artist *a = &g_artists[idx];
            char t[40]; truncate_str(a->title, t, 36);
            if (a->album_count == 1)
                snprintf(label, sizeof(label), "%s  (%d tracks)",
                         t, g_albums[a->album_start].track_count);
            else
                snprintf(label, sizeof(label), "%s  (%d albums)", t, a->album_count);
            is_playing = (idx == app.play_artist);
        } else if (app.view == VIEW_ALBUMS) {
            int ai = g_artists[app.cur_artist].album_start + idx;
            Album *a = &g_albums[ai];
            char t[40]; truncate_str(a->title, t, 36);
            if (a->saved_position_ms > 0) {
                char ts[16]; format_time(a->saved_position_ms, ts, sizeof(ts));
                snprintf(label, sizeof(label), "%s  [%d/%d %s]",
                         t, a->saved_track + 1, a->track_count, ts);
            } else {
                snprintf(label, sizeof(label), "%s  (%d tracks)", t, a->track_count);
            }
            is_playing = (ai == app.play_album);
        } else {
            Album *a = &g_albums[app.cur_album];
            Track *t = &g_tracks[a->track_start + idx];
            char tn[54]; truncate_str(t->title, tn, 50);
            snprintf(label, sizeof(label), "%3d. %s", idx + 1, tn);
            is_playing = (a->track_start + idx == app.play_track);
        }

        SDL_Color col;
        if (is_playing) col = rgb(0x44, 0xCC, 0x66);
        else if (idx == app.selected) col = rgb(0xEE, 0xEE, 0xEE);
        else col = rgb(0x88, 0x88, 0x99);

        char disp[90];
        if (is_playing)
            snprintf(disp, sizeof(disp), "%s %s", app.is_paused ? "||" : ">", label);
        else
            snprintf(disp, sizeof(disp), "   %s", label);

        draw_text(app.font_small, disp, col, 8, y + 14);
    }

    if (scroll + vis < count)
        draw_text(app.font_small, "v", rgb(0x4F, 0xC3, 0xF7), SCREEN_W-24, SCREEN_H-44);

    // Error
    if (app.error_msg[0]) {
        fill_rect(20, SCREEN_H/2-30, SCREEN_W-40, 60,
                  SDL_MapRGB(app.screen->format, 0x40, 0x10, 0x10));
        draw_text(app.font_small, app.error_msg,
                  rgb(0xEF, 0x44, 0x44), 40, SCREEN_H/2-14);
    }

    // Footer
    fill_rect(0, SCREEN_H-36, SCREEN_W, 36,
              SDL_MapRGB(app.screen->format, 0x11, 0x11, 0x22));
    const char *hint;
    if (app.locked) {
        hint = S()->unlock_hold;
        draw_text_center(app.font_small, hint, rgb(0x55, 0x55, 0x66), SCREEN_W/2, SCREEN_H-26);
    } else {
        if (app.view == VIEW_ARTISTS)
            hint = S()->hint_artists;
        else if (app.view == VIEW_ALBUMS)
            hint = S()->hint_albums;
        else
            hint = S()->hint_tracks;
        draw_text(app.font_small, hint, rgb(0x55, 0x55, 0x66), 10, SCREEN_H-26);
    }

    if (app.locked) render_lock_indicator();
    if (app.lock_confirm) render_lock_confirm();
    SDL_Flip(app.screen);
}

// ============================================================
//  Input
// ============================================================

static void handle_input(SDL_Event *e) {
    if (e->type == SDL_QUIT) { app.running = 0; return; }
    if (e->type != SDL_KEYDOWN) return;

    // If screen is off, first keypress only wakes it up
    if (g_screen_off) {
        screen_on();
        return;  // swallow this keypress
    }
    screen_on();  // reset idle timer on every keypress

    app.error_msg[0] = '\0';
    int count = item_count();

    switch (e->key.keysym.sym) {

    case SDLK_UP:
        if (count > 0)
            app.selected = (app.selected > 0) ? app.selected - 1 : count - 1;
        break;

    case SDLK_DOWN:
        if (count > 0)
            app.selected = (app.selected < count-1) ? app.selected + 1 : 0;
        break;

    case SDLK_LEFT:  // page up
        if (count > 0) {
            app.selected -= app.visible_rows;
            if (app.selected < 0) app.selected = 0;
        }
        break;

    case SDLK_RIGHT:  // page down
        if (count > 0) {
            app.selected += app.visible_rows;
            if (app.selected >= count) app.selected = count - 1;
        }
        break;

    case SDLK_SPACE:  // A
        if (app.view == VIEW_ARTISTS && g_artist_total > 0) {
            Artist *art = &g_artists[app.selected];
            app.cur_artist = app.selected;
            if (art->album_count == 1) {
                // Skip album list if only one album
                app.cur_album = art->album_start;
                app.view = VIEW_TRACKS;
                Album *alb = &g_albums[app.cur_album];
                app.selected = (alb->saved_track < alb->track_count)
                               ? alb->saved_track : 0;
                app.scroll_offset = 0;
            } else {
                app.view = VIEW_ALBUMS;
                app.selected = 0;
                app.scroll_offset = 0;
            }
        } else if (app.view == VIEW_ALBUMS) {
            int ai = g_artists[app.cur_artist].album_start + app.selected;
            app.cur_album = ai;
            app.view = VIEW_TRACKS;
            Album *alb = &g_albums[ai];
            app.selected = (alb->saved_track < alb->track_count)
                           ? alb->saved_track : 0;
            app.scroll_offset = 0;
        } else if (app.view == VIEW_TRACKS) {
            Album *alb = cur_album_ptr();
            if (alb->track_count > 0) {
                int global_idx = alb->track_start + app.selected;
                if (global_idx == app.play_track && app.cur_album == app.play_album) {
                    // Already playing this track – just jump to Now Playing
                    app.view = VIEW_NOWPLAYING;
                } else {
                    int seek = 0;
                    if (app.selected == alb->saved_track && alb->saved_position_ms > 0) {
                        seek = alb->saved_position_ms - 3000;
                        if (seek < 0) seek = 0;
                    }
                    do_play(app.cur_artist, app.cur_album, global_idx, seek);
                    app.view = VIEW_NOWPLAYING;
                }
            }
        }
        break;

    case SDLK_RETURN:  // Start – toggle Now Playing <-> browse
        if (app.play_album >= 0) {
            // Save current browse position and jump to Now Playing
            app.saved_view = app.view;
            app.saved_selected = app.selected;
            app.cur_artist = app.play_artist;
            app.cur_album = app.play_album;
            app.view = VIEW_NOWPLAYING;
        }
        break;

    case SDLK_LCTRL:  // B – back or quit confirm
        if (app.view == VIEW_TRACKS) {
            if (g_artists[app.cur_artist].album_count == 1) {
                app.view = VIEW_ARTISTS;
                app.selected = app.cur_artist;
            } else {
                app.view = VIEW_ALBUMS;
                app.selected = app.cur_album - g_artists[app.cur_artist].album_start;
            }
            app.scroll_offset = 0;
        } else if (app.view == VIEW_ALBUMS) {
            app.view = VIEW_ARTISTS;
            app.selected = app.cur_artist;
            app.scroll_offset = 0;
        } else if (app.view == VIEW_ARTISTS) {
            // Show quit confirmation
            if (app.play_album >= 0 && !app.is_paused)
                toggle_pause();  // pause playback
            app.show_quit_confirm = 1;
        }
        break;

    case SDLK_e:         seek_rel(-15); break;   // L1
    case SDLK_t:         seek_rel(+15); break;   // R1
    // L2/R2 reserved for lock combo – physical volume buttons handle audio

    default: break;
    }
}

// ============================================================
//  NTP time sync
// ============================================================

// ============================================================
//  Timezone auto-detection via ip-api.com
//  GET /line/?fields=offset  →  plain-text UTC offset in seconds
// ============================================================

static void tz_sync(void) {
    log_msg("TZ sync: starting");

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { log_msg("TZ sync: socket failed"); return; }

    struct timeval tv;
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(80);

    struct hostent *host = gethostbyname("ip-api.com");
    if (host) {
        memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
    } else {
        log_msg("TZ sync: DNS failed, using fallback IP");
        addr.sin_addr.s_addr = inet_addr("208.95.112.1");
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("TZ sync: connect failed (errno=%d)", errno);
        close(sock);
        return;
    }

    const char *req =
        "GET /line/?fields=offset HTTP/1.0\r\n"
        "Host: ip-api.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (send(sock, req, strlen(req), 0) < 0) {
        log_msg("TZ sync: send failed");
        close(sock);
        return;
    }

    // Read full response (headers + body)
    char buf[512];
    int total = 0, n;
    while (total < (int)sizeof(buf) - 1 &&
           (n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0)) > 0)
        total += n;
    close(sock);

    if (total <= 0) { log_msg("TZ sync: no response"); return; }
    buf[total] = '\0';

    // Body is after \r\n\r\n
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) { log_msg("TZ sync: malformed response"); return; }
    body += 4;

    int offset = atoi(body);
    g_tz_offset_sec = offset;
    log_msg("TZ sync OK: offset=%d s (UTC%+.1f)", offset, (double)offset / 3600.0);
}

// Fallback NTP server IPs if DNS is unavailable
// time.cloudflare.com = 162.159.200.1, time.google.com = 216.239.35.0
static const char *ntp_fallback_ips[] = {
    "216.239.35.0",   // time.google.com
    "162.159.200.1",  // time.cloudflare.com
};

static int ntp_query(struct sockaddr_in *addr) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { log_msg("NTP: socket failed (errno=%d)", errno); return 0; }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3

    if (sendto(sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        log_msg("NTP: sendto failed (errno=%d)", errno);
        close(sock);
        return 0;
    }

    int n = recv(sock, pkt, sizeof(pkt), 0);
    close(sock);
    if (n < 48) { log_msg("NTP: no response (n=%d)", n); return 0; }

    unsigned long ntp_secs = ((unsigned long)pkt[40] << 24) |
                             ((unsigned long)pkt[41] << 16) |
                             ((unsigned long)pkt[42] << 8)  |
                             ((unsigned long)pkt[43]);
    time_t unix_secs = (time_t)(ntp_secs - 2208988800UL);
    struct timeval new_time;
    new_time.tv_sec  = unix_secs;
    new_time.tv_usec = 0;
    if (settimeofday(&new_time, NULL) == 0) {
        log_msg("NTP sync OK: %ld", (long)unix_secs);
        return 1;
    }
    log_msg("NTP: settimeofday failed (errno=%d)", errno);
    return 0;
}

static void ntp_sync(void) {
    log_msg("NTP sync: starting");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(123);

    // Try DNS first
    struct hostent *host = gethostbyname("pool.ntp.org");
    if (host) {
        log_msg("NTP sync: DNS resolved pool.ntp.org");
        memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
        if (ntp_query(&addr)) return;
    } else {
        log_msg("NTP sync: DNS failed (h_errno=%d), trying fallback IPs", h_errno);
    }

    // DNS failed or server unresponsive — try hardcoded IPs
    for (int i = 0; i < (int)(sizeof(ntp_fallback_ips)/sizeof(ntp_fallback_ips[0])); i++) {
        addr.sin_addr.s_addr = inet_addr(ntp_fallback_ips[i]);
        log_msg("NTP sync: trying fallback %s", ntp_fallback_ips[i]);
        if (ntp_query(&addr)) return;
    }

    log_msg("NTP sync: all servers failed");
}

// ============================================================
//  Signal handler – graceful shutdown on SIGTERM / SIGHUP
// ============================================================

static volatile sig_atomic_t g_signal_received = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_signal_received = 1;
}

// ============================================================
//  Main
// ============================================================

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    log_init();
    log_msg("App starting");

    signal(SIGTERM, signal_handler);
    signal(SIGHUP,  signal_handler);

    memset(&app, 0, sizeof(app));
    app.play_artist = -1;
    app.play_album  = -1;
    app.play_track  = -1;
    app.running     = 1;
    app.volume      = VOL_MAX;
    app.view        = VIEW_ARTISTS;
    app.speed_idx   = SPEED_DEFAULT;
    app.cover_art   = NULL;
    app.cover_album_id = -1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        log_msg("SDL_Init FAILED: %s", SDL_GetError());
        log_close(); return 1;
    }
    if (TTF_Init() < 0) {
        log_msg("TTF_Init FAILED"); SDL_Quit(); log_close(); return 1;
    }
    // Open audio device directly (bypass SDL_mixer — its callbacks don't fire on Miyoo)
    {
        SDL_AudioSpec desired, obtained;
        memset(&desired, 0, sizeof(desired));
        desired.freq = 44100;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 2048;
        desired.callback = audio_callback;
        desired.userdata = NULL;

        for (int attempt = 0; attempt < 3 && !app.audio_ok; attempt++) {
            if (attempt > 0) SDL_Delay(200);
            if (SDL_OpenAudio(&desired, &obtained) < 0) {
                log_msg("SDL_OpenAudio attempt %d FAILED: %s", attempt + 1, SDL_GetError());
            } else {
                app.audio_ok = 1;
                log_msg("Audio OK: %d Hz, %d ch, %d samples",
                        obtained.freq, obtained.channels, obtained.samples);
            }
        }
    }
    if (app.audio_ok) {
        SDL_PauseAudio(0);
        load_mpg123();
    }

    app.screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16,
                                  SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!app.screen)
        app.screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!app.screen) { log_msg("Video FAILED"); goto cleanup; }

    SDL_ShowCursor(SDL_DISABLE);
    SDL_EnableKeyRepeat(KEY_REPEAT_DELAY, KEY_REPEAT_INTERVAL);

    SDL_FillRect(app.screen, NULL, 0); SDL_Flip(app.screen);
    SDL_FillRect(app.screen, NULL, 0); SDL_Flip(app.screen);

    app.font_large = TTF_OpenFont(FONT_PATH, 28);
    app.font_small = TTF_OpenFont(FONT_PATH, 18);
    if (!app.font_large || !app.font_small) {
        log_msg("Font FAILED: %s", TTF_GetError()); goto cleanup;
    }

    app.visible_rows = (SCREEN_H - 62 - 44) / 50;
    log_msg("Visible rows: %d", app.visible_rows);

    // Save current brightness for later restore
    g_saved_brightness = read_int_file(PWM_DUTY);
    log_msg("Saved brightness: %d", g_saved_brightness);

    load_settings();
    draw_startup();
    scan_library();
    load_progress();
    ntp_sync();
    tz_sync();
    g_last_input = SDL_GetTicks();
    log_msg("Main loop");

    Uint32 frame_delay = 1000 / FPS;
    Uint32 last_save = 0;
    SDL_Event e;

    while (app.running && !g_signal_received) {
        Uint32 start = SDL_GetTicks();

        // L2+R2 hold detection → show lock/unlock prompt (confirmed with A)
        {
            Uint8 *keys = SDL_GetKeyState(NULL);
            int l2r2 = keys[SDLK_TAB] && keys[SDLK_BACKSPACE]; // L2=TAB R2=BACKSPACE
            if (l2r2) {
                if (app.lock_combo_since == 0)
                    app.lock_combo_since = SDL_GetTicks();
                else if (SDL_GetTicks() - app.lock_combo_since > 1000) {
                    if (!app.lock_confirm) {
                        // Show lock/unlock confirm dialog
                        app.lock_confirm = 1;
                        app.lock_combo_since = 0;
                        if (app.locked && g_screen_off) screen_on();
                    }
                }
            } else {
                app.lock_combo_since = 0;
            }
        }

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { app.running = 0; break; }

            // Lock confirm dialog: A confirms, B cancels
            if (app.lock_confirm) {
                if (e.type == SDL_KEYDOWN) {
                    if (g_screen_off) { screen_on(); continue; }  // wake only
                    screen_on();
                    if (e.key.keysym.sym == SDLK_SPACE) {  // A = confirm
                        if (!app.locked) {
                            app.locked = 1;
                            app.lock_confirm = 0;
                            screen_off();
                            log_msg("Locked");
                        } else {
                            app.locked = 0;
                            app.lock_confirm = 0;
                            screen_on();
                            log_msg("Unlocked");
                        }
                    } else if (e.key.keysym.sym == SDLK_LCTRL) {  // B = cancel
                        app.lock_confirm = 0;
                    }
                }
                continue;
            }

            if (app.locked) {
                // While locked, only allow screen wake-up
                if (e.type == SDL_KEYDOWN && g_screen_off) screen_on();
                continue;
            }
            // Select button opens settings from anywhere
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RCTRL
                && !app.show_quit_confirm && !app.show_resume) {
                if (g_screen_off) { screen_on(); }
                else {
                    screen_on();
                    g_show_options = !g_show_options;
                    g_option_selected = 0;
                    if (g_show_options) g_path_idx_on_enter = g_path_idx;
                }
                continue;
            }

            if (app.show_quit_confirm)
                handle_quit_confirm(&e);
            else if (g_show_about)
                handle_about(&e);
            else if (g_show_options)
                handle_options(&e);
            else if (app.show_resume)
                handle_resume_input(&e);
            else if (app.view == VIEW_NOWPLAYING)
                handle_nowplaying(&e);
            else
                handle_input(&e);
        }

        if (!app.show_resume) {
            if (app.play_album >= 0 && !app.is_paused) {
                int done = g_decoder ? (g_music_done && g_ring_avail == 0)
                                     : !Mix_PlayingMusic();
                if (done) play_next();
            }

            if (app.play_album >= 0 && !app.is_paused &&
                SDL_GetTicks() - last_save > 10000) {
                save_progress();
                last_save = SDL_GetTicks();
            }
        }

        // Sleep timer: auto-pause after configured minutes
        if (app.sleep_timer_min > 0 && app.play_album >= 0 && !app.is_paused) {
            int elapsed_s = (int)(SDL_GetTicks() - app.sleep_timer_start) / 1000;
            if (elapsed_s >= app.sleep_timer_min * 60) {
                toggle_pause();
                save_progress();
                app.sleep_timer_min = 0;
                log_msg("Sleep timer: paused");
            }
        }

        // Auto screen-off when idle
        int timeout = timeout_values[g_timeout_idx];
        if (!g_screen_off && timeout > 0 &&
            SDL_GetTicks() - g_last_input > (Uint32)timeout) {
            screen_off();
            log_msg("Screen off (idle)");
        }

        // Apply deferred seek (SDL_mixer fallback only)
        if (g_pending_seek_ms >= 0 && app.play_album >= 0 && !g_decoder) {
            Mix_SetMusicPosition(g_pending_seek_ms / 1000.0);
            if (!app.is_paused)
                app.play_start_ticks = SDL_GetTicks();
            g_pending_seek_ms = -1;
        }

        // Skip rendering when screen is off (saves CPU too)
        if (!g_screen_off) {
            // Lock confirm overlay takes priority over everything
            if (app.lock_confirm)
                render_lock_confirm();
            else if (app.show_quit_confirm)
                render_quit_confirm();
            else if (g_show_about)
                render_about();
            else if (g_show_options)
                render_options();
            else if (app.show_resume)
                render_resume();
            else if (app.view == VIEW_NOWPLAYING)
                render_nowplaying();
            else
                render();
        }

        Uint32 el = SDL_GetTicks() - start;
        if (el < frame_delay) SDL_Delay(frame_delay - el);
    }

cleanup:
    // 0. Allow system to sleep again
    stay_awake_set(0);

    // 1. Stop audio callback first (prevents deadlock during cleanup)
    if (app.audio_ok) SDL_PauseAudio(1);

    // 2. Stop decoder (safe now that callback is paused)
    decoder_close();
    if (app.music) { Mix_HaltMusic(); Mix_FreeMusic(app.music); app.music = NULL; }

    // 3. Clear BOTH framebuffers to black (needs SDL fully operational)
    if (app.screen) {
        SDL_FillRect(app.screen, NULL, 0); SDL_Flip(app.screen);
        SDL_FillRect(app.screen, NULL, 0); SDL_Flip(app.screen);
    }

    // 4. Restore backlight if screen was off
    if (g_screen_off) screen_on();

    // 5. Save progress
    save_progress();

    // 6. Now close audio subsystem (after all SDL_Flip calls are done)
    if (app.audio_ok) { SDL_CloseAudio(); app.audio_ok = 0; }
    if (g_mpg123_lib) { mp3_exit(); dlclose(g_mpg123_lib); g_mpg123_lib = NULL; }

    // 6. Free graphics resources
    if (app.cover_art) { SDL_FreeSurface(app.cover_art); app.cover_art = NULL; }
    if (app.font_large) { TTF_CloseFont(app.font_large); app.font_large = NULL; }
    if (app.font_small) { TTF_CloseFont(app.font_small); app.font_small = NULL; }
    TTF_Quit();

    SDL_Quit();
    log_msg("Done");
    log_close();
    return 0;
}
