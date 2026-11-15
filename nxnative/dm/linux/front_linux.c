// Linux front end (dm/host/front.h) on SDL3: the counterpart of dm/android/front_android.c.
//   dmhost <data dir> [-- game arguments]
// <data dir> is the data pack porter makes for Android (mods/, RomFS/, shaders.dmsv, env.txt). Saves go to <data dir>/save, scratch files to <data dir>/temp.
// The main thread owns SDL: the window (its Vulkan surface goes to gfx_vk_present.c through dm_front_vk_surface) and the controllers, which feed player 1's pad.
// The host (dm/host/main.c, built as dm_host_main) runs on a thread of its own with a 16 MB stack, as on Android.
// DM_HEADLESS=1: no window and no SDL, frames counted only (gfx_vk_present.c's headless path, DM_VK_SHOT).
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_host_main(int argc, char **argv);   // dm/host/main.c (main, renamed by dm/linux/build.py)

// ---- host front end interface ----
void front_run(void (*guest)(void)) { guest(); }   // already on the host thread
void *front_metal_layer(void) { return NULL; }
void front_screen_match(int fps) { (void)fps; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)max_chars;
    snprintf(out, (size_t)out_cap, "%s", initial ? initial : "");   // no keyboard yet: the game's text stays as it offered it
    return 1;
}

// ---- the window ----
static SDL_Window *g_win;
static _Atomic int32_t g_win_w, g_win_h;
static _Atomic int g_max_hz;
int front_screen_max_fps(void) { return atomic_load(&g_max_hz); }
ANativeWindow *dm_front_window(void) { return (ANativeWindow *)g_win; }
int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return atomic_load(&g_win_w); }
int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return atomic_load(&g_win_h); }
uint32_t dm_front_vk_instance_exts(const char **out, uint32_t cap) {
    if (!g_win) return 0;
    Uint32 n = 0; const char *const *e = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!e) { LOG("[RED] SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError()); return 0; }
    if (n > cap) n = cap;
    for (Uint32 i = 0; i < n; i++) out[i] = e[i];
    return n;
}
int dm_front_vk_surface(VkInstance inst, VkSurfaceKHR *out) {
    if (!g_win) return 0;
    if (!SDL_Vulkan_CreateSurface(g_win, inst, NULL, out)) { LOG("[RED] SDL_Vulkan_CreateSurface: %s", SDL_GetError()); return 0; }
    return 1;
}
static void win_size(void) { int w = 0, h = 0; SDL_GetWindowSizeInPixels(g_win, &w, &h); atomic_store(&g_win_w, w); atomic_store(&g_win_h, h); }

// ---- controllers: every one feeds player 1, by position (the bottom face button is B, the right one A, as the Android and iOS front ends map) ----
static uint64_t g_kbd;   // the keyboard's buttons (main thread)
static int32_t axis(Sint16 v) { int a = v < -32767 ? -32767 : v; return a > -4000 && a < 4000 ? 0 : a; }
static uint64_t pad_buttons(SDL_Gamepad *g) {
    static const struct { SDL_GamepadButton b; uint64_t bit; } M[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, DM_BTN_B}, {SDL_GAMEPAD_BUTTON_EAST, DM_BTN_A}, {SDL_GAMEPAD_BUTTON_WEST, DM_BTN_Y}, {SDL_GAMEPAD_BUTTON_NORTH, DM_BTN_X},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DM_BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DM_BTN_R}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, DM_BTN_LS},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, DM_BTN_RS}, {SDL_GAMEPAD_BUTTON_START, DM_BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, DM_BTN_MINUS},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, DM_BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DM_BTN_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DM_BTN_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT}};
    uint64_t b = 0;
    for (unsigned i = 0; i < sizeof M / sizeof *M; i++) if (SDL_GetGamepadButton(g, M[i].b)) b |= M[i].bit;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= DM_BTN_ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= DM_BTN_ZR;
    return b;
}
static void pad_publish(void) {
    uint64_t b = g_kbd; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]); if (!g) continue;
        b |= pad_buttons(g);
        int32_t x = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)), y = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));   // SDL's y grows downward, the Switch's upward
        int32_t u = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX)), v = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
        if (abs(x) > abs(lx)) lx = x; if (abs(y) > abs(ly)) ly = y; if (abs(u) > abs(rx)) rx = u; if (abs(v) > abs(ry)) ry = v;
    }
    SDL_free(ids);
    dm_pad_set(b, lx, ly, rx, ry);
}
static uint64_t key_bit(SDL_Keycode k) {   // a keyboard for desktop runs: arrows, Z/X = B/A, A/S = Y/X, Q/W = L/R, 1/2 = ZL/ZR, Enter = +, Backspace = -
    switch (k) {
    case SDLK_UP: return DM_BTN_UP; case SDLK_DOWN: return DM_BTN_DOWN; case SDLK_LEFT: return DM_BTN_LEFT; case SDLK_RIGHT: return DM_BTN_RIGHT;
    case SDLK_Z: return DM_BTN_B; case SDLK_X: return DM_BTN_A; case SDLK_A: return DM_BTN_Y; case SDLK_S: return DM_BTN_X;
    case SDLK_Q: return DM_BTN_L; case SDLK_W: return DM_BTN_R; case SDLK_1: return DM_BTN_ZL; case SDLK_2: return DM_BTN_ZR;
    case SDLK_RETURN: return DM_BTN_PLUS; case SDLK_BACKSPACE: return DM_BTN_MINUS;
    default: return 0;
    }
}
// Guide + Start held together quits (a handheld has no window to close)
static int quit_combo(void) {
    int n = 0, q = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && !q; i++) { SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        q = g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE) && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START); }
    SDL_free(ids); return q;
}

// ---- the host ----
static char g_root[PATH_MAX];
static int g_hargc; static char **g_hargv;
static void *host_thread(void *a) {
    (void)a;
    int rc = dm_host_main(g_hargc, g_hargv);
    LOG("host returned %d", rc);
    _exit(rc);
}
static void env_file(const char *path) {   // KEY=VALUE per line (# comments), as the Android app's env.txt
    FILE *f = fopen(path, "r"); char line[1024];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0; setenv(line, eq + 1, 1);
    }
    fclose(f);
}
extern _Thread_local void *dm_tls_slot_app;   // tls_slot.c
static void tls_slot_check(void) {
    void **tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if ((void *)&dm_tls_slot_app != (void *)(tp + 2))
        die("the guest thread slot is not at tpidr_el0[2] (%p, tpidr_el0 %p): dm/linux/tls_slot.c must be the first object of the link", (void *)&dm_tls_slot_app, (void *)tp);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <data dir> [-- game arguments]\n", argv[0]); return 2; }
    tls_slot_check();
    if (!realpath(argv[1], g_root)) { fprintf(stderr, "%s: %s\n", argv[1], strerror(errno)); return 2; }
    char p[PATH_MAX + 32];
    setenv("DM_ROOT", g_root, 1);
    snprintf(p, sizeof p, "%s/RomFS", g_root); setenv("DM_ROMFS", p, 0);
    snprintf(p, sizeof p, "%s/save", g_root); mkdir(p, 0700); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", g_root); mkdir(p, 0700); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/shaders.dmsv", g_root); setenv("DM_VK_SHADERS", p, 0);
    snprintf(p, sizeof p, "%s/env.txt", g_root); env_file(p);
#ifdef DM_RELEASE
    unsetenv("DM_CTL");
#endif
    // the host's arguments: the module folder, then whatever followed (-- game arguments)
    g_hargv = calloc((size_t)argc + 2, sizeof *g_hargv); g_hargv[g_hargc++] = argv[0];
    snprintf(p, sizeof p, "%s/mods", g_root); g_hargv[g_hargc++] = strdup(p);
    for (int i = 2; i < argc; i++) g_hargv[g_hargc++] = argv[i];

    const char *hl = getenv("DM_HEADLESS"); int headless = hl && atoi(hl);
    if (!headless) {
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm) atomic_store(&g_max_hz, (int)(dm->refresh_rate + 0.5f));
        const char *ws = getenv("DM_WINDOWED"); int windowed = ws && atoi(ws);
        g_win = SDL_CreateWindow("nx2linux", dm && !windowed ? dm->w : 1280, dm && !windowed ? dm->h : 720, SDL_WINDOW_VULKAN | (windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN));
        if (!g_win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
        SDL_HideCursor(); win_size();
    }
    LOG("NX2A host on Linux: files %s, RomFS %s, saves %s, window %dx%d", g_root, getenv("DM_ROMFS"), getenv("DM_SAVE"), atomic_load(&g_win_w), atomic_load(&g_win_h));

    // the game's main thread: a 16 MB stack mapped here and handed over whole, with a guard page below (front_android.c: IL2CPP's collector scans the reported range)
    size_t ssz = 16 << 20, guard = 64 << 10;
    uint8_t *stk = mmap(NULL, ssz + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    pthread_attr_t at; pthread_attr_init(&at); pthread_t t;
    if (stk != MAP_FAILED) { mprotect(stk, guard, PROT_NONE); pthread_attr_setstack(&at, stk + guard, ssz); } else pthread_attr_setstacksize(&at, ssz);
    if (pthread_create(&t, &at, host_thread, NULL)) die("cannot start the host thread");
    pthread_attr_destroy(&at);
    if (headless) { pthread_join(t, NULL); return 0; }

    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) { if (quit_combo()) _exit(0); continue; }
        switch (e.type) {
        case SDL_EVENT_QUIT: _exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: win_size(); break;
        case SDL_EVENT_GAMEPAD_ADDED: { SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which); LOG("controller: %s", g ? SDL_GetGamepadName(g) : SDL_GetError()); pad_publish(); break; }
        case SDL_EVENT_GAMEPAD_REMOVED: { SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which); if (g) SDL_CloseGamepad(g); pad_publish(); break; }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (quit_combo()) _exit(0);
            pad_publish(); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            if (e.key.key == SDLK_ESCAPE && e.type == SDL_EVENT_KEY_DOWN) _exit(0);
            if (e.type == SDL_EVENT_KEY_DOWN) g_kbd |= key_bit(e.key.key); else g_kbd &= ~key_bit(e.key.key);
            pad_publish(); break;
        default: break;
        }
    }
}
