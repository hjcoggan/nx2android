#define _GNU_SOURCE
// Does an mmap of an msm dma-buf at a non-zero offset map that offset? Allocates a 4-page GEM buffer, writes each page's index through a
// mapping at offset 0, then maps page 2 alone (offset 8192) and reads it: 2 = offsets honoured, 0 = the mapping starts at page 0.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/msm_drm.h>
int main(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR); if (fd < 0) { perror("renderD128"); return 1; }
    struct drm_msm_gem_new n = {.size = 4 * 4096, .flags = MSM_BO_WC};
    if (ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, &n)) { perror("GEM_NEW"); return 1; }
    struct drm_prime_handle p = {.handle = n.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p)) { perror("PRIME"); return 1; }
    uint32_t *all = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, 0); if (all == MAP_FAILED) { perror("mmap 0"); return 1; }
    for (int i = 0; i < 4; i++) all[i * 1024] = i;
    uint32_t *two = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, 8192); if (two == MAP_FAILED) { perror("mmap 8192"); return 1; }
    printf("page at offset 8192 reads %u (2: offsets honoured, 0: ignored)\n", two[0]);
    // the workaround: a populated mapping at offset 0, its page 2 moved where it belongs with mremap (the page table entries move with it)
    uint32_t *pop = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, p.fd, 0); if (pop == MAP_FAILED) { perror("mmap populate"); return 1; }
    uint32_t *dst = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t *mv = mremap((uint8_t *)pop + 8192, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    if (mv == MAP_FAILED) { perror("mremap"); return 1; }
    printf("moved page reads %u (want 2)\n", mv[0]);
    mprotect(mv, 4096, PROT_READ); mprotect(mv, 4096, PROT_READ | PROT_WRITE); mv[1] = 0xabcd;
    printf("after mprotect RO/RW: %u, write seen through offset-0 mapping: %#x (want 2, 0xabcd)\n", mv[0], all[2 * 1024 + 1]);
    return 0;
}
