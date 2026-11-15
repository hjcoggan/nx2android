// Linux build: the window the Vulkan backend presents to is the front end's SDL window (front_linux.c), passed around as an opaque ANativeWindow.
#pragma once
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
int32_t ANativeWindow_getWidth(ANativeWindow *w);    // pixels
int32_t ANativeWindow_getHeight(ANativeWindow *w);
