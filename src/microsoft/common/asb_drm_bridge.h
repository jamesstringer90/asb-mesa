#ifndef ASB_DRM_BRIDGE_H
#define ASB_DRM_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "util/format/u_formats.h"

#include "asb_drm_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct asb_drm_bridge;
struct asb_drm_buffer;

bool asb_drm_query_adapter(int fd, uint64_t *adapter_luid,
                           struct drm_asb_gpu_query *info);
uint32_t asb_drm_format(uint32_t capabilities, enum pipe_format format);
uint32_t asb_drm_bridge_format(struct asb_drm_bridge *bridge,
                              enum pipe_format format);
bool asb_drm_bridge_has_modifiers(struct asb_drm_bridge *bridge);
bool asb_drm_format_compatible(uint32_t format, uint32_t imported_format);
int asb_drm_open_matching_render_node(uint64_t adapter_luid, dev_t *device_id);
bool asb_drm_primary_node(dev_t render_device, dev_t *primary_device);

struct asb_drm_image {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t pitch;
};

struct asb_drm_bridge *asb_drm_bridge_create(int fd, uint64_t adapter_luid,
                                          int fence_fd);
void asb_drm_bridge_destroy(struct asb_drm_bridge *bridge);
struct asb_drm_buffer *asb_drm_buffer_create(struct asb_drm_bridge *bridge,
                                           const struct asb_drm_image *image);
void asb_drm_buffer_destroy(struct asb_drm_buffer *buffer);
uint32_t asb_drm_buffer_handle(struct asb_drm_buffer *buffer);
int asb_drm_buffer_fd(struct asb_drm_buffer *buffer);
bool asb_drm_import_image(struct asb_drm_bridge *bridge, int fd,
                          struct asb_drm_image *image);
int asb_drm_export_fence(struct asb_drm_bridge *bridge, uint64_t value);
int asb_drm_fence_sync_file(struct asb_drm_bridge *bridge, int fence_fd,
                            uint64_t value);
bool asb_drm_buffer_set_fence(struct asb_drm_buffer *buffer, int fd, bool write);
bool asb_drm_buffer_wait(struct asb_drm_buffer *buffer, bool write);
bool asb_drm_dma_buf_set_fence(int dma_buf_fd, int sync_fd, bool write);
bool asb_drm_dma_buf_wait(int dma_buf_fd, bool write);

#ifdef __cplusplus
}
#endif

#endif
