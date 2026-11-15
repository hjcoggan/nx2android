// Linux build: the AHardwareBuffer calls the Vulkan backend makes for zero-copy pools (gfx_vk_core.c), on Vulkan memory exported as a dma-buf (ahb_vk.c).
#pragma once
#include <stdint.h>
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
enum { AHARDWAREBUFFER_FORMAT_BLOB = 0x21 };
enum { AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3ull, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN = 3ull << 4, AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER = 1ull << 24 };
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out);
void AHardwareBuffer_release(AHardwareBuffer *b);
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *b, int fd);
