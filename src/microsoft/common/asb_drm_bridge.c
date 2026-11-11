#include "asb_drm_bridge.h"
#include "asb_drm_uapi.h"
#include "d3dkmthk.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/format/u_format.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct asb_drm_bridge {
    int fd;
    int dxg_fd;
    int fence_fd;
    uint64_t adapter_luid;
    uint32_t device_handle;
    uint32_t fence_handle;
    uint32_t capabilities;
    uint32_t max_width;
    uint32_t max_height;
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

bool asb_drm_query_adapter(int fd, uint64_t *adapter_luid,
                           struct drm_asb_gpu_query *info)
{
    const uint32_t required = ASB_DRM_GPU_CAP_BUFFER_IMPORT |
                              ASB_DRM_GPU_CAP_NATIVE_HANDLES |
                              ASB_DRM_GPU_CAP_SYNC_FILE |
                              ASB_DRM_GPU_CAP_PRESENT;
    struct drm_asb_gpu_query query = {0};

    if (fd < 0 || asb_drm_ioctl(fd, DRM_IOCTL_ASB_GPU_QUERY, &query) < 0 ||
        query.version != ASB_DRM_GPU_VERSION ||
        (query.capabilities & required) != required)
        return false;
    if (adapter_luid)
        *adapter_luid = query.adapter_luid;
    if (info)
        *info = query;
    return true;
}

uint32_t asb_drm_format(uint32_t capabilities, enum pipe_format format)
{
    format = util_format_linear(format);
    if (format == PIPE_FORMAT_B8G8R8A8_UNORM)
        return DRM_FORMAT_ARGB8888;
    if (format == PIPE_FORMAT_B8G8R8X8_UNORM)
        return DRM_FORMAT_XRGB8888;
    if (!(capabilities & ASB_DRM_GPU_CAP_RGB_FORMATS))
        return 0;

    switch (format) {
    case PIPE_FORMAT_R8G8B8A8_UNORM: return DRM_FORMAT_ABGR8888;
    case PIPE_FORMAT_R8G8B8X8_UNORM: return DRM_FORMAT_XBGR8888;
    case PIPE_FORMAT_R10G10B10A2_UNORM: return DRM_FORMAT_ABGR2101010;
    case PIPE_FORMAT_R10G10B10X2_UNORM: return DRM_FORMAT_XBGR2101010;
    case PIPE_FORMAT_R16G16B16A16_UNORM: return DRM_FORMAT_ABGR16161616;
    case PIPE_FORMAT_R16G16B16X16_UNORM: return DRM_FORMAT_XBGR16161616;
    case PIPE_FORMAT_R16G16B16A16_FLOAT: return DRM_FORMAT_ABGR16161616F;
    case PIPE_FORMAT_R16G16B16X16_FLOAT: return DRM_FORMAT_XBGR16161616F;
    case PIPE_FORMAT_R32G32B32A32_FLOAT: return DRM_FORMAT_ABGR32323232F;
    case PIPE_FORMAT_B5G6R5_UNORM: return DRM_FORMAT_RGB565;
    case PIPE_FORMAT_B5G5R5A1_UNORM: return DRM_FORMAT_ARGB1555;
    case PIPE_FORMAT_B5G5R5X1_UNORM: return DRM_FORMAT_XRGB1555;
    case PIPE_FORMAT_B4G4R4A4_UNORM: return DRM_FORMAT_ARGB4444;
    case PIPE_FORMAT_B4G4R4X4_UNORM: return DRM_FORMAT_XRGB4444;
    case PIPE_FORMAT_A4B4G4R4_UNORM: return DRM_FORMAT_RGBA4444;
    case PIPE_FORMAT_R8_UNORM: return DRM_FORMAT_R8;
    case PIPE_FORMAT_R8G8_UNORM: return DRM_FORMAT_GR88;
    case PIPE_FORMAT_R16_UNORM: return DRM_FORMAT_R16;
    case PIPE_FORMAT_R16G16_UNORM: return DRM_FORMAT_GR1616;
    case PIPE_FORMAT_R16_FLOAT: return DRM_FORMAT_R16F;
    case PIPE_FORMAT_R16G16_FLOAT: return DRM_FORMAT_GR1616F;
    case PIPE_FORMAT_R32_FLOAT: return DRM_FORMAT_R32F;
    case PIPE_FORMAT_R32G32_FLOAT: return DRM_FORMAT_GR3232F;
    default: return 0;
    }
}

uint32_t asb_drm_bridge_format(struct asb_drm_bridge *bridge,
                              enum pipe_format format)
{
    return bridge ? asb_drm_format(bridge->capabilities, format) : 0;
}

bool asb_drm_bridge_has_modifiers(struct asb_drm_bridge *bridge)
{
    return bridge && (bridge->capabilities & ASB_DRM_GPU_CAP_MODIFIERS);
}

bool asb_drm_format_compatible(uint32_t format, uint32_t imported_format)
{
    if (!format || !imported_format)
        return false;
    if (format == imported_format)
        return true;
    switch (format) {
    case DRM_FORMAT_ARGB8888: return imported_format == DRM_FORMAT_XRGB8888;
    case DRM_FORMAT_XRGB8888: return imported_format == DRM_FORMAT_ARGB8888;
    case DRM_FORMAT_ABGR8888: return imported_format == DRM_FORMAT_XBGR8888;
    case DRM_FORMAT_XBGR8888: return imported_format == DRM_FORMAT_ABGR8888;
    case DRM_FORMAT_ABGR2101010: return imported_format == DRM_FORMAT_XBGR2101010;
    case DRM_FORMAT_XBGR2101010: return imported_format == DRM_FORMAT_ABGR2101010;
    case DRM_FORMAT_ABGR16161616: return imported_format == DRM_FORMAT_XBGR16161616;
    case DRM_FORMAT_XBGR16161616: return imported_format == DRM_FORMAT_ABGR16161616;
    case DRM_FORMAT_ABGR16161616F: return imported_format == DRM_FORMAT_XBGR16161616F;
    case DRM_FORMAT_XBGR16161616F: return imported_format == DRM_FORMAT_ABGR16161616F;
    case DRM_FORMAT_ARGB1555: return imported_format == DRM_FORMAT_XRGB1555;
    case DRM_FORMAT_XRGB1555: return imported_format == DRM_FORMAT_ARGB1555;
    case DRM_FORMAT_ARGB4444: return imported_format == DRM_FORMAT_XRGB4444;
    case DRM_FORMAT_XRGB4444: return imported_format == DRM_FORMAT_ARGB4444;
    default: return false;
    }
}

int asb_drm_open_matching_render_node(uint64_t adapter_luid, dev_t *device_id)
{
    DIR *dir = opendir("/dev/dri");
    struct dirent *entry;
    int found = -1;

    if (!dir)
        return -1;
    while ((entry = readdir(dir))) {
        char path[128];
        struct stat statbuf;
        uint64_t found_luid;
        int fd;

        if (strncmp(entry->d_name, "renderD", 7) ||
            strspn(entry->d_name + 7, "0123456789") !=
                strlen(entry->d_name + 7) ||
            !entry->d_name[7] ||
            snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name) >=
                sizeof(path))
            continue;
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (fstat(fd, &statbuf) == 0 && S_ISCHR(statbuf.st_mode) &&
            asb_drm_query_adapter(fd, &found_luid, NULL) &&
            found_luid == adapter_luid) {
            if (device_id)
                *device_id = statbuf.st_rdev;
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

bool asb_drm_primary_node(dev_t render_device, dev_t *primary_device)
{
    char path[128];
    DIR *dir;
    struct dirent *entry;
    bool found = false;

    if (snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/drm",
                 major(render_device), minor(render_device)) >= sizeof(path))
        return false;
    dir = opendir(path);
    if (!dir)
        return false;
    while (!found && (entry = readdir(dir))) {
        char node[128];
        struct stat statbuf;

        if (strncmp(entry->d_name, "card", 4) || !entry->d_name[4] ||
            strspn(entry->d_name + 4, "0123456789") != strlen(entry->d_name + 4) ||
            snprintf(node, sizeof(node), "/dev/dri/%s", entry->d_name) >= sizeof(node))
            continue;
        if (stat(node, &statbuf) == 0 && S_ISCHR(statbuf.st_mode)) {
            *primary_device = statbuf.st_rdev;
            found = true;
        }
    }
    closedir(dir);
    return found;
}

struct asb_drm_bridge *asb_drm_bridge_create(int fd, uint64_t adapter_luid,
                                          int fence_fd)
{
    struct drm_asb_gpu_native native = {0};
    struct asb_drm_bridge *bridge;
    uint64_t found_luid;
    struct drm_asb_gpu_query query;

    if (!asb_drm_query_adapter(fd, &found_luid, &query) ||
        found_luid != adapter_luid)
        return NULL;
    bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return NULL;
    bridge->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    bridge->dxg_fd = open("/dev/dxg", O_RDWR | O_CLOEXEC);
    bridge->fence_fd = fcntl(fence_fd, F_DUPFD_CLOEXEC, 0);
    bridge->adapter_luid = adapter_luid;
    bridge->capabilities = query.capabilities;
    bridge->max_width = query.max_width;
    bridge->max_height = query.max_height;
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

    if (!image->width || image->width > bridge->max_width ||
        !image->height || image->height > bridge->max_height)
        return NULL;
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
    return asb_drm_fence_sync_file(bridge, bridge->fence_fd, value);
}

int asb_drm_fence_sync_file(struct asb_drm_bridge *bridge, int fence_fd,
                            uint64_t value)
{
    struct drm_asb_gpu_native native = {
        .dxg_fd = bridge->dxg_fd,
        .fd = fence_fd,
    };
    struct d3dkmt_createsyncfile sync = {.fence_value = value};

    if (fence_fd == bridge->fence_fd) {
        native.device_handle = bridge->device_handle;
        native.object_handle = bridge->fence_handle;
        native.adapter_luid = bridge->adapter_luid;
    } else if (fence_fd < 0 ||
               asb_drm_ioctl(bridge->fd, DRM_IOCTL_ASB_GPU_NATIVE, &native) < 0) {
        return -1;
    }
    if (native.adapter_luid != bridge->adapter_luid)
        return -1;
    sync.device.v = native.device_handle;
    sync.monitored_fence.v = native.object_handle;
    if (asb_drm_ioctl(bridge->dxg_fd, LX_DXCREATESYNCFILE, &sync) < 0)
        return -1;
    return (int)sync.sync_file_handle;
}

bool asb_drm_dma_buf_set_fence(int dma_buf_fd, int sync_fd, bool write)
{
    struct dma_buf_import_sync_file sync = {
        .flags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ,
        .fd = sync_fd,
    };
    return asb_drm_ioctl(dma_buf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &sync) == 0;
}

bool asb_drm_dma_buf_wait(int dma_buf_fd, bool write)
{
    struct dma_buf_export_sync_file sync = {
        .flags = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ,
        .fd = -1,
    };
    struct timespec start, now;
    bool result = false;

    if (asb_drm_ioctl(dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) < 0)
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

bool asb_drm_buffer_set_fence(struct asb_drm_buffer *buffer, int fd, bool write)
{
    return asb_drm_dma_buf_set_fence(buffer->fd, fd, write);
}

bool asb_drm_buffer_wait(struct asb_drm_buffer *buffer, bool write)
{
    return asb_drm_dma_buf_wait(buffer->fd, write);
}
