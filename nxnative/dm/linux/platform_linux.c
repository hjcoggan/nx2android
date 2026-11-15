// Linux build: what the Android-only sources of dm/android provide, where Linux has nothing to do or nothing yet.
//   AAsset: the APK's assets as files under $DM_ROOT/assets.
//   Performance hints (perf_android.c): none.
//   H.264 movies (movie_avc.c, MediaCodec): no decoder yet; each access unit gives a black picture, so a movie plays out its length and the game goes on.
#include "dm.h"
#include "hle_movie_avc.h"
#include <android/asset_manager.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AAsset { uint8_t *p; size_t n, at; };
AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode) {
    (void)m; (void)mode;
    const char *root = getenv("DM_ROOT"); if (!root) return NULL;
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/assets/%s", root, name);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    AAsset *a = calloc(1, sizeof *a);
    if (a && !fseek(f, 0, SEEK_END)) { long n = ftell(f); rewind(f);
        if (n >= 0 && (a->p = malloc((size_t)n + 1)) && fread(a->p, 1, (size_t)n, f) == (size_t)n) a->n = (size_t)n;
        else { free(a->p); free(a); a = NULL; } }
    fclose(f); return a;
}
off_t AAsset_getLength(AAsset *a) { return (off_t)a->n; }
int AAsset_read(AAsset *a, void *buf, size_t n) { size_t k = a->n - a->at < n ? a->n - a->at : n; memcpy(buf, a->p + a->at, k); a->at += k; return (int)k; }
void AAsset_close(AAsset *a) { if (a) { free(a->p); free(a); } }

void dm_perf_window(ANativeWindow *w, double fps) { (void)w; (void)fps; }
void dm_perf_frame(double period) { (void)period; }
void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns) { (void)replay_ns; (void)gpu_ns; }
void dm_perf_gpu_release(void) {}
void dm_perf_game_mode(int mode) { (void)mode; }

#define QMAX 16
struct mavc { int64_t q[QMAX]; int nq, rgba; };
mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); LOG("[RED] movie: no H.264 decoder on Linux yet: black pictures"); return a; }
void mavc_drop(mavc *a) { a->nq = 0; }
void mavc_free(mavc *a) { free(a); }
int mavc_queued(const mavc *a) { return a->nq; }
int mavc_held(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) { (void)p; if (n > 0 && a->nq < QMAX) a->q[a->nq++] = pts_us; return 0; }
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    (void)flush;
    if (!a->nq) return 0;
    if (pts_us) *pts_us = a->q[0];
    memmove(a->q, a->q + 1, (size_t)--a->nq * sizeof a->q[0]);
    if (a->rgba) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    else { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    return 1;
}
