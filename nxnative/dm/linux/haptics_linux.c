// Linux build: vibration on SDL3's gamepad rumble, the counterpart of dm/android/haptics_android.c. Player 1's rumble (hle_haptics.c's state) and the
// pause menu's taps go to the controller in use (the last one that sent input: on a handheld, its built-in gamepad, whose motor its driver exposes).
// The state is handed over under hle_haptics.c's lock and played on a thread of its own, every TICK_NS, with the same envelope as Android's: the strongest
// level the game set since the last tick, then a fall of FALL a tick once it goes quiet (Switch 2 games stream HD rumble as short peaks between near-silence).
// The low and high motors keep their own strengths: SDL drives a controller's two motors as the Switch's low- and high-frequency ones.
#include "dm.h"
#include "hle_haptics.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define TICK_NS 40000000ll
#define FALL 0.6f
#define TAP_MS 30
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static dm_haptic_state g_state; static float g_peak[2]; static int g_taps, g_clear;
static _Atomic int g_on = 1; static _Atomic SDL_JoystickID g_pad;
static int64_t mono_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static float level(const dm_vibration *v, int high) { dm_haptic_level l = dm_haptics_level(v, 1, 1.0f); (void)high; return l.intensity; }
static void sink(dm_haptic_state s) {   // hle_haptics.c, under its lock: only hand the state over
    if (s.player) return;
    pthread_mutex_lock(&g_mu);
    if (s.epoch != g_state.epoch) { g_peak[0] = g_peak[1] = 0; g_clear = 1; }
    g_state = s;
    for (int m = 0; m < 2; m++) { float lv = level(&s.motor[m], m); if (lv > g_peak[m]) g_peak[m] = lv; }
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}
static void rumble(float lo, float hi, int ms) {
    SDL_Gamepad *g = SDL_GetGamepadFromID(atomic_load(&g_pad)); if (!g) return;
    SDL_RumbleGamepad(g, (Uint16)(lo * 65535), (Uint16)(hi * 65535), (Uint32)ms);
}
static void *haptics_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "haptics");
    dm_haptics_route(0, 1);
    float env[2] = {0, 0}; int playing = 0; int64_t next = 0, tap_end = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        for (;;) {
            if (g_taps || g_clear) break;
            if (!playing && g_peak[0] <= 0 && g_peak[1] <= 0) { pthread_cond_wait(&g_cv, &g_mu); continue; }
            int64_t t = mono_now(); if (t >= next) break;
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); int64_t w = next - t; dl.tv_sec += w / 1000000000; dl.tv_nsec += w % 1000000000;
            if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &dl);
        }
        int taps = g_taps, clear = g_clear; dm_haptic_state s = g_state; float peak[2] = {g_peak[0], g_peak[1]};
        int64_t t = mono_now(); int tick = t >= next;
        g_taps = g_clear = 0; if (tick) g_peak[0] = g_peak[1] = 0;
        pthread_mutex_unlock(&g_mu);
        int on = atomic_load(&g_on);
        if (clear) { env[0] = env[1] = 0; if (playing) { rumble(0, 0, 0); playing = 0; } }
        if (taps && on) { rumble(0.25f, 0.5f, TAP_MS); tap_end = t + TAP_MS * 1000000ll; }
        if (!tick) continue;
        next = t + TICK_NS;
        for (int m = 0; m < 2; m++) {
            float now = level(&s.motor[m], m), want = peak[m] > now ? peak[m] : now;
            env[m] = want >= env[m] ? want : env[m] * FALL > want ? env[m] * FALL : want;
            if (env[m] < 4 / 255.0f) env[m] = 0;
        }
        if (!on || (!env[0] && !env[1])) { if (playing) { rumble(0, 0, 0); playing = 0; } continue; }
        if (t < tap_end) continue;
        if (!playing) { static int first = 1; if (first) { first = 0; LOG("[haptics] first rumble from the game: low %.2f high %.2f", env[0], env[1]); } }
        rumble(env[0], env[1], (int)(TICK_NS / 1000000) * 3);   // three ticks long: the next tick renews it before it ends
        playing = 1;
    }
    return NULL;
}
void dm_haptics_linux_start(int on) {
    atomic_store(&g_on, on != 0);
    dm_haptics_init(sink, 1);
    pthread_t t; if (!pthread_create(&t, NULL, haptics_thread, NULL)) pthread_detach(t);
}
void dm_haptics_linux_set(int on) { atomic_store(&g_on, on != 0); pthread_mutex_lock(&g_mu); g_clear = !on; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
void dm_haptics_linux_pad(SDL_JoystickID id) { atomic_store(&g_pad, id); }
void dm_haptics_linux_tap(void) { if (!atomic_load(&g_on)) return; pthread_mutex_lock(&g_mu); g_taps++; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
