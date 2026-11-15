// Linux build: the APK's assets are files under $DM_ROOT/assets (android_shim.c), read whole as AASSET_MODE_BUFFER does.
#pragma once
#include <sys/types.h>
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
enum { AASSET_MODE_BUFFER = 3 };
AAsset *AAssetManager_open(AAssetManager *m, const char *name, int mode);
off_t AAsset_getLength(AAsset *a);
int AAsset_read(AAsset *a, void *buf, size_t n);
void AAsset_close(AAsset *a);
