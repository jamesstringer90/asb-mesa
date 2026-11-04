#ifndef ASB_DRM_BRIDGE_H
#define ASB_DRM_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct asb_drm_bridge;
struct asb_drm_buffer;

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
bool asb_drm_buffer_set_fence(struct asb_drm_buffer *buffer, int fd, bool write);
bool asb_drm_buffer_wait(struct asb_drm_buffer *buffer, bool write);

#ifdef __cplusplus
}
#endif

#endif
