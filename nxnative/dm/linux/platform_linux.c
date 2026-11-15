// Linux build: what the Android-only sources of dm/android provide, where Linux has nothing to do or nothing yet.
//   AAsset: the APK's assets as files under $DM_ROOT/assets.
//   Performance hints (perf_android.c): none.
#include "dm.h"
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
