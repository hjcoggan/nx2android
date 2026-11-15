// Linux build: the AHardwareBuffer calls of the zero-copy memory pools (gfx_vk_core.c) on Vulkan memory exported as a dma-buf.
// AHardwareBuffer_allocate allocates device memory the CPU sees cached and coherent where the driver has it (Turnip on Adreno 650: DEVICE_LOCAL | HOST_VISIBLE |
// HOST_COHERENT | HOST_CACHED) and exports it; the backend mmaps the dma-buf over the pool's pages (MAP_SHARED | MAP_FIXED) and imports it again as a buffer's
// memory (dm_ahb_import, through gfx_vk_core.c ahb_import), so the game's pointer is the GPU's memory, as AHardwareBuffer does on Android.
#include "gfx_vk.h"
#include <android/hardware_buffer.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct AHardwareBuffer { VkDeviceMemory mem; int fd; uint64_t size; };

static PFN_vkGetMemoryFdKHR p_get_fd;
static PFN_vkGetMemoryFdPropertiesKHR p_fd_props;
static uint32_t g_type = UINT32_MAX;   // the memory type exported buffers come from
static int ready(void) {
    static int st; if (st) return st > 0;
    p_get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(V.dev, "vkGetMemoryFdKHR");
    p_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(V.dev, "vkGetMemoryFdPropertiesKHR");
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int pass = 0; pass < 2 && g_type == UINT32_MAX; pass++)   // cached first: the CPU reads the game's own memory through this mapping
        for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = V.mem.memoryTypes[i].propertyFlags, want = hv | (pass ? 0 : VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if ((f & want) == want && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { g_type = i; break; }
        }
    st = p_get_fd && p_fd_props && g_type != UINT32_MAX ? 1 : -1;
    if (st > 0) VKLOG("zero-copy pools: dma-buf exports of memory type %u (flags %#x)", g_type, V.mem.memoryTypes[g_type].propertyFlags);
    else VKLOG("[RED] zero-copy pools: no dma-buf export (vkGetMemoryFdKHR %s, a host-visible coherent type %s): every pool is copied", p_get_fd ? "yes" : "no", g_type != UINT32_MAX ? "yes" : "no");
    return st > 0;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out) {
    *out = NULL;
    if (!ready()) return -ENOSYS;
    AHardwareBuffer *b = calloc(1, sizeof *b); if (!b) return -ENOMEM;
    b->size = d->width; b->fd = -1;
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, b->size, g_type};
    VkResult r = vkAllocateMemory(V.dev, &ai, NULL, &b->mem);
    if (!r) { VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, b->mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT}; r = p_get_fd(V.dev, &gi, &b->fd); }
    if (r) { if (b->mem) vkFreeMemory(V.dev, b->mem, NULL); free(b); return r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY ? -ENOMEM : -EIO; }
    *out = b; return 0;
}
void AHardwareBuffer_release(AHardwareBuffer *b) {
    if (!b) return;
    if (b->fd >= 0) close(b->fd);
    vkFreeMemory(V.dev, b->mem, NULL);   // the pages stay while a mapping or an imported memory holds the dma-buf
    free(b);
}
// the dma-buf's file as SCM_RIGHTS (gfx_vk_core.c recv_fds takes the first file of the message, as of Android's native handle)
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *b, int sock) {
    char data = 0, ctl[CMSG_SPACE(sizeof(int))]; struct iovec iov = {&data, 1};
    struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &b->fd, sizeof(int));
    return sendmsg(sock, &m, 0) < 0 ? -errno : 0;
}
VkResult dm_ahb_import(AHardwareBuffer *b, VkDeviceMemory *out) {
    int fd = dup(b->fd); if (fd < 0) return VK_ERROR_TOO_MANY_OBJECTS;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = p_fd_props(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
    uint32_t type = !r && (fp.memoryTypeBits & (1u << g_type)) ? g_type : vk_mem_type(fp.memoryTypeBits, 0, 0);
    VkImportMemoryFdInfoKHR im = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &im, b->size, type};
    if (!r) r = vkAllocateMemory(V.dev, &ai, NULL, out);   // the driver owns fd once this succeeds
    if (r) { close(fd); *out = VK_NULL_HANDLE; }
    return r;
}
