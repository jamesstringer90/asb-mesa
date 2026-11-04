#include "asb_drm_bridge.h"
#include "asb_drm_uapi.h"
#include "d3dkmthk.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct asb_drm_bridge {
    int fd;
    int dxg_fd;
    int fence_fd;
    uint64_t adapter_luid;
    uint32_t device_handle;
    uint32_t fence_handle;
    atomic_uint references;
};

struct asb_drm_buffer {
    struct asb_drm_bridge *bridge;
    uint32_t handle;
    int fd;
};

static int asb_drm_ioctl(int fd, unsigned long request, void *data)
{
    int result;
    do {
        result = ioctl(fd, request, data);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void asb_drm_close_handle(int fd, uint32_t handle)
{
    struct drm_gem_close close_handle = {.handle = handle};
    asb_drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
}

struct asb_drm_bridge *asb_drm_bridge_create(int fd, uint64_t adapter_luid,
                                          int fence_fd)
{
    struct drm_asb_gpu_query query = {0};
    struct drm_asb_gpu_native native = {0};
    struct asb_drm_bridge *bridge;
    const uint32_t required = ASB_DRM_GPU_CAP_BUFFER_IMPORT |
                              ASB_DRM_GPU_CAP_NATIVE_HANDLES |
                              ASB_DRM_GPU_CAP_SYNC_FILE |
                              ASB_DRM_GPU_CAP_PRESENT;

    if (fd < 0 || asb_drm_ioctl(fd, DRM_IOCTL_ASB_GPU_QUERY, &query) < 0 ||
        query.version != ASB_DRM_GPU_VERSION ||
        query.adapter_luid != adapter_luid ||
        (query.capabilities & required) != required)
        return NULL;
    bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return NULL;
    bridge->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    bridge->dxg_fd = open("/dev/dxg", O_RDWR | O_CLOEXEC);
    bridge->fence_fd = fcntl(fence_fd, F_DUPFD_CLOEXEC, 0);
    bridge->adapter_luid = adapter_luid;
    atomic_init(&bridge->references, 1);
    native.dxg_fd = bridge->dxg_fd;
    native.fd = bridge->fence_fd;
    if (bridge->fd < 0 || bridge->dxg_fd < 0 || bridge->fence_fd < 0 ||
        asb_drm_ioctl(bridge->fd, DRM_IOCTL_ASB_GPU_NATIVE, &native) < 0 ||
        native.adapter_luid != adapter_luid) {
        asb_drm_bridge_destroy(bridge);
        return NULL;
    }
    bridge->device_handle = native.device_handle;
    bridge->fence_handle = native.object_handle;
    return bridge;
}

void asb_drm_bridge_destroy(struct asb_drm_bridge *bridge)
{
    if (!bridge || atomic_fetch_sub_explicit(&bridge->references, 1,
                                            memory_order_acq_rel) != 1)
        return;
    if (bridge->fence_fd >= 0)
        close(bridge->fence_fd);
    if (bridge->dxg_fd >= 0)
        close(bridge->dxg_fd);
    if (bridge->fd >= 0)
        close(bridge->fd);
    free(bridge);
}

struct asb_drm_buffer *asb_drm_buffer_create(struct asb_drm_bridge *bridge,
                                           const struct asb_drm_image *image)
{
    struct drm_asb_gpu_import imported = {
        .fd = image->fd,
        .width = image->width,
        .height = image->height,
        .format = image->format,
        .pitch = image->pitch,
    };
    struct drm_prime_handle prime = {.flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1};
    struct asb_drm_buffer *buffer;

    if (asb_drm_ioctl(bridge->fd, DRM_IOCTL_ASB_GPU_IMPORT, &imported) < 0)
        return NULL;
    prime.handle = imported.handle;
    if (imported.adapter_luid != bridge->adapter_luid ||
        asb_drm_ioctl(bridge->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
        asb_drm_close_handle(bridge->fd, imported.handle);
        return NULL;
    }
    buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        close(prime.fd);
        asb_drm_close_handle(bridge->fd, imported.handle);
        return NULL;
    }
    buffer->bridge = bridge;
    buffer->handle = imported.handle;
    buffer->fd = prime.fd;
    atomic_fetch_add_explicit(&bridge->references, 1, memory_order_relaxed);
    return buffer;
}

void asb_drm_buffer_destroy(struct asb_drm_buffer *buffer)
{
    if (!buffer)
        return;
    close(buffer->fd);
    asb_drm_close_handle(buffer->bridge->fd, buffer->handle);
    asb_drm_bridge_destroy(buffer->bridge);
    free(buffer);
}

uint32_t asb_drm_buffer_handle(struct asb_drm_buffer *buffer)
{
    return buffer->handle;
}

int asb_drm_buffer_fd(struct asb_drm_buffer *buffer)
{
    return fcntl(buffer->fd, F_DUPFD_CLOEXEC, 0);
}

bool asb_drm_import_image(struct asb_drm_bridge *bridge, int fd,
                          struct asb_drm_image *image)
{
    struct drm_asb_gpu_export exported = {
        .flags = ASB_DRM_GPU_EXPORT_PRIME_FD,
        .fd = fd,
    };
    if (asb_drm_ioctl(bridge->fd, DRM_IOCTL_ASB_GPU_EXPORT, &exported) < 0)
        return false;
    if (exported.adapter_luid != bridge->adapter_luid) {
        close(exported.fd);
        return false;
    }
    image->fd = exported.fd;
    image->width = exported.width;
    image->height = exported.height;
    image->format = exported.format;
    image->pitch = exported.pitch;
    return true;
}

int asb_drm_export_fence(struct asb_drm_bridge *bridge, uint64_t value)
{
    struct d3dkmt_createsyncfile sync = {.fence_value = value};

    sync.device.v = bridge->device_handle;
    sync.monitored_fence.v = bridge->fence_handle;
    if (asb_drm_ioctl(bridge->dxg_fd, LX_DXCREATESYNCFILE, &sync) < 0)
        return -1;
    return (int)sync.sync_file_handle;
}

bool asb_drm_buffer_set_fence(struct asb_drm_buffer *buffer, int fd, bool write)
{
    struct dma_buf_import_sync_file sync = {
        .flags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ,
        .fd = fd,
    };
    return asb_drm_ioctl(buffer->fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &sync) == 0;
}

bool asb_drm_buffer_wait(struct asb_drm_buffer *buffer, bool write)
{
    struct dma_buf_export_sync_file sync = {
        .flags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ,
        .fd = -1,
    };
    struct timespec start, now;
    bool result = false;

    if (asb_drm_ioctl(buffer->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) < 0)
        return false;
    struct pollfd pending = {.fd = sync.fd, .events = POLLIN};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = 2000 - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                      (now.tv_nsec - start.tv_nsec) / 1000000);
        if (remaining < 0) remaining = 0;
        int status = poll(&pending, 1, remaining);
        if (status > 0) {
            result = (pending.revents & (POLLIN | POLLERR | POLLNVAL)) == POLLIN;
            break;
        }
        if (!status || errno != EINTR || !remaining) break;
    }
    close(sync.fd);
    return result;
}
