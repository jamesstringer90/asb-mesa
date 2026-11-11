/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dzn_private.h"

#include "vk_alloc.h"
#include "vk_debug_report.h"
#include "vk_util.h"

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_atomic.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#ifndef _WIN32
#include <sys/eventfd.h>
#include "util/libsync.h"
#include <poll.h>
#include <unistd.h>
#include "c11/threads.h"
#include "vk_sync_binary.h"
#endif

static VkResult
dzn_sync_init(struct vk_device *device,
              struct vk_sync *sync,
              uint64_t initial_value)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);

   D3D12_FENCE_FLAGS flags = (sync->flags & VK_SYNC_IS_SHAREABLE) ?
      D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE;

   if (FAILED(ID3D12Device1_CreateFence(ddev->dev, initial_value,
                                        flags,
                                        &IID_ID3D12Fence,
                                        (void **)&dsync->fence)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   return VK_SUCCESS;
}

static void
dzn_sync_finish(struct vk_device *device,
                struct vk_sync *sync)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);

   ID3D12Fence_Release(dsync->fence);

#ifdef _WIN32
   if (dsync->export_handle)
      CloseHandle(dsync->export_handle);
#endif
}

static VkResult
dzn_sync_signal(struct vk_device *device,
                struct vk_sync *sync,
                uint64_t value)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);

   if (!(sync->flags & VK_SYNC_IS_TIMELINE))
      value = 1;

   if (FAILED(ID3D12Fence_Signal(dsync->fence, value)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   return VK_SUCCESS;
}

static VkResult
dzn_sync_get_value(struct vk_device *device,
                   struct vk_sync *sync,
                   uint64_t *value)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);

   *value = ID3D12Fence_GetCompletedValue(dsync->fence);
   return VK_SUCCESS;
}

static VkResult
dzn_sync_reset(struct vk_device *device,
               struct vk_sync *sync)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);

   if (FAILED(ID3D12Fence_Signal(dsync->fence, 0)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   return VK_SUCCESS;
}

static VkResult
dzn_sync_move(struct vk_device *device,
              struct vk_sync *dst,
              struct vk_sync *src)
{
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);
   struct dzn_sync *ddst = container_of(dst, struct dzn_sync, vk);
   struct dzn_sync *dsrc = container_of(src, struct dzn_sync, vk);
   ID3D12Fence *new_fence;

   if (FAILED(ID3D12Device1_CreateFence(ddev->dev, 0,
                                        D3D12_FENCE_FLAG_NONE,
                                        &IID_ID3D12Fence,
                                        (void **)&new_fence)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   ID3D12Fence_Release(ddst->fence);
   ddst->fence = dsrc->fence;
   dsrc->fence = new_fence;
   return VK_SUCCESS;
}

static VkResult
dzn_sync_wait(struct vk_device *device,
              uint32_t wait_count,
              const struct vk_sync_wait *waits,
              enum vk_sync_wait_flags wait_flags,
              uint64_t abs_timeout_ns)
{
   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;

   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);

#ifdef _WIN32
   HANDLE event = CreateEventA(NULL, false, false, NULL);
   if (event == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
#else
   int event_fd = eventfd(0, EFD_CLOEXEC);
   if (event_fd == -1)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   /* The D3D12 event-based APIs in WSL expect an eventfd file descriptor cast to HANDLE */
   HANDLE event = (HANDLE)(intptr_t)event_fd;
#endif

   STACK_ARRAY(ID3D12Fence *, fences, wait_count);
   STACK_ARRAY(uint64_t, values, wait_count);

   for (uint32_t i = 0; i < wait_count; i++) {
      struct dzn_sync *sync = container_of(waits[i].sync, struct dzn_sync, vk);

      fences[i] = sync->fence;
      values[i] = (sync->vk.flags & VK_SYNC_IS_TIMELINE) ? waits[i].wait_value : 1;
   }

   D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags =
      (wait_flags & VK_SYNC_WAIT_ANY) ?
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY :
      D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL;

   if (FAILED(ID3D12Device1_SetEventOnMultipleFenceCompletion(ddev->dev,
                                                              fences,
                                                              values,
                                                              wait_count,
                                                              flags,
                                                              event))) {
      STACK_ARRAY_FINISH(fences);
      STACK_ARRAY_FINISH(values);
#ifdef _WIN32
      CloseHandle(event);
#else
      close(event_fd);
#endif
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

#ifdef _WIN32
   DWORD timeout_ms;
   static const DWORD timeout_infinite = INFINITE;
#else
   int timeout_ms;
   static const int timeout_infinite = -1;
#endif

   if (abs_timeout_ns == OS_TIMEOUT_INFINITE) {
      timeout_ms = timeout_infinite;
   } else {
      uint64_t cur_time = os_time_get_nano();
      uint64_t rel_timeout_ns =
         abs_timeout_ns > cur_time ? abs_timeout_ns - cur_time : 0;

      timeout_ms = (rel_timeout_ns / 1000000) + (rel_timeout_ns % 1000000 ? 1 : 0);
   }

#ifdef _WIN32
   DWORD res =
      WaitForSingleObject(event, timeout_ms);

   CloseHandle(event);
   VkResult ret = VK_SUCCESS;
   if (res == WAIT_TIMEOUT)
      ret = VK_TIMEOUT;
   else if (res != WAIT_OBJECT_0)
      ret = vk_error(device, VK_ERROR_UNKNOWN);
#else
   VkResult ret = sync_wait(event_fd, timeout_ms) != 0 ? VK_TIMEOUT : VK_SUCCESS;
   close(event_fd);
#endif

   STACK_ARRAY_FINISH(fences);
   STACK_ARRAY_FINISH(values);

   return ret;
}

#ifdef _WIN32
static VkResult
dzn_sync_import_win32_handle(struct vk_device *device, struct vk_sync *sync,
                             HANDLE handle, LPCWSTR name)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);

   dzn_sync_finish(device, sync);

   HANDLE handle_to_close = NULL;
   if (name) {
      if (FAILED(ID3D12Device_OpenSharedHandleByName(ddev->dev, name, GENERIC_ALL, &handle_to_close)))
         return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      handle = handle_to_close;
   }

   HRESULT hr = ID3D12Device_OpenSharedHandle(ddev->dev, handle, &IID_ID3D12Fence, (void **)&dsync->fence);
   if (handle_to_close)
      CloseHandle(handle_to_close);
   if (FAILED(hr))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   return VK_SUCCESS;
}

static VkResult
dzn_sync_export_win32_handle(struct vk_device *device, struct vk_sync *sync, HANDLE *handle)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);
   if (dsync->export_handle) {
      if (!DuplicateHandle(GetCurrentProcess(), dsync->export_handle,
                           GetCurrentProcess(), handle,
                           0, false, DUPLICATE_SAME_ACCESS))
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return VK_SUCCESS;
   }

   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);
   if (FAILED(ID3D12Device_CreateSharedHandle(ddev->dev, (ID3D12DeviceChild *)dsync->fence,
                                              NULL, GENERIC_ALL, NULL, handle)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   return VK_SUCCESS;
}

static VkResult
dzn_sync_prep_win32_export(struct vk_device *device, struct vk_sync *sync,
                           const void *security_attributes, uint32_t access,
                           const wchar_t *name)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);
   if (FAILED(ID3D12Device_CreateSharedHandle(ddev->dev, (ID3D12DeviceChild *)dsync->fence,
                                              (const LPSECURITY_ATTRIBUTES)security_attributes,
                                              GENERIC_ALL, name, &dsync->export_handle)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   return VK_SUCCESS;
}
#else
static VkResult
dzn_sync_import_opaque_fd(struct vk_device *device, struct vk_sync *sync,
                          int fd)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);

   dzn_sync_finish(device, sync);

   HANDLE handle = (HANDLE)(intptr_t)fd;
   if (FAILED(ID3D12Device_OpenSharedHandle(ddev->dev, handle, &IID_ID3D12Fence, (void **)&dsync->fence)))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   return VK_SUCCESS;
}

static VkResult
dzn_sync_export_opaque_fd(struct vk_device *device, struct vk_sync *sync, int *fd)
{
   struct dzn_sync *dsync = container_of(sync, struct dzn_sync, vk);
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);
   HANDLE handle;
   if (FAILED(ID3D12Device_CreateSharedHandle(ddev->dev, (ID3D12DeviceChild *)dsync->fence,
                                              NULL, GENERIC_ALL, NULL, &handle)))
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   *fd = (int)(intptr_t)handle;
   return VK_SUCCESS;
}
#endif

#ifndef _WIN32
static struct dzn_sync *
dzn_sync_binary_timeline(struct vk_sync *sync, uint64_t *point)
{
   struct vk_sync_binary *binary = container_of(sync, struct vk_sync_binary, sync);
   *point = binary->next_point;
   return container_of(&binary->timeline, struct dzn_sync, vk);
}

static VkResult
dzn_sync_binary_export_sync_file(struct vk_device *device,
                                 struct vk_sync *sync,
                                 int *fd)
{
   uint64_t point;
   struct dzn_sync *dsync = dzn_sync_binary_timeline(sync, &point);

   *fd = -1;
   if (ID3D12Fence_GetCompletedValue(dsync->fence) >= point)
      return VK_SUCCESS;

#ifdef HAVE_ASB_DRM
   struct dzn_device *ddev = container_of(device, struct dzn_device, vk);
   if (ddev->asb_bridge) {
      HANDLE handle;
      if (FAILED(ID3D12Device_CreateSharedHandle(ddev->dev,
                                                    (ID3D12DeviceChild *)dsync->fence,
                                                    NULL, GENERIC_ALL, NULL,
                                                    &handle)))
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      *fd = asb_drm_fence_sync_file(ddev->asb_bridge, (int)(intptr_t)handle, point);
      close((int)(intptr_t)handle);
      return *fd >= 0 ? VK_SUCCESS : vk_error(device, VK_ERROR_UNKNOWN);
   }
#endif

   return vk_sync_wait(device, &container_of(sync, struct vk_sync_binary, sync)->timeline,
                       point, 0, OS_TIMEOUT_INFINITE);
}

struct dzn_sync_file_waiter {
   struct list_head link;
   struct dzn_device *device;
   thrd_t thread;
   int fd;
   int cancel_fd;
   int done;
   ID3D12Fence *fence;
   uint64_t point;
};

static int
dzn_sync_file_waiter_run(void *data)
{
   struct dzn_sync_file_waiter *waiter = data;
   struct pollfd pending[] = {
      {.fd = waiter->fd, .events = POLLIN},
      {.fd = waiter->cancel_fd, .events = POLLIN},
   };
   int ret;
   do {
      ret = poll(pending, ARRAY_SIZE(pending), -1);
   } while (ret < 0 && errno == EINTR);
   if (!pending[1].revents) {
      if (ret < 0 || !(pending[0].revents & POLLIN) ||
          (pending[0].revents & (POLLERR | POLLNVAL)))
         vk_device_set_lost(&waiter->device->vk, "Sync-file wait failed");
      if (FAILED(ID3D12Fence_Signal(waiter->fence, waiter->point)))
         vk_device_set_lost(&waiter->device->vk, "Sync-file fence signal failed");
   }
   ID3D12Fence_Release(waiter->fence);
   close(waiter->fd);
   p_atomic_set(&waiter->done, 1);
   return 0;
}

static void
dzn_sync_file_waiter_destroy(struct dzn_sync_file_waiter *waiter)
{
   thrd_join(waiter->thread, NULL);
   close(waiter->cancel_fd);
   list_del(&waiter->link);
   free(waiter);
}

VkResult
dzn_sync_device_init(struct dzn_device *device)
{
   if (mtx_init(&device->sync_file_lock, mtx_plain) != thrd_success)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   list_inithead(&device->sync_file_waiters);
   device->sync_file_initialized = true;
   return VK_SUCCESS;
}

void
dzn_sync_device_finish(struct dzn_device *device)
{
   if (!device->sync_file_initialized)
      return;
   list_for_each_entry_safe(struct dzn_sync_file_waiter, waiter,
                            &device->sync_file_waiters, link) {
      uint64_t cancel = 1;
      ASSERTED ssize_t written;
      do {
         written = write(waiter->cancel_fd, &cancel, sizeof(cancel));
      } while (written < 0 && errno == EINTR);
      assert(written == sizeof(cancel) || errno == EAGAIN);
      dzn_sync_file_waiter_destroy(waiter);
   }
   mtx_destroy(&device->sync_file_lock);
}

static VkResult
dzn_sync_binary_import_sync_file(struct vk_device *device,
                                 struct vk_sync *sync,
                                 int fd)
{
   uint64_t point;
   struct dzn_sync *dsync = dzn_sync_binary_timeline(sync, &point);

   if (!sync_valid_fd(fd))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   struct pollfd pending = {.fd = fd, .events = POLLIN};
   int ret;
   do {
      ret = poll(&pending, 1, 0);
   } while (ret < 0 && errno == EINTR);
   if (ret < 0 || (pending.revents & (POLLERR | POLLNVAL)))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   if (ret == 1 && (pending.revents & POLLIN)) {
      if (FAILED(ID3D12Fence_Signal(dsync->fence, point)))
         return vk_error(device, VK_ERROR_DEVICE_LOST);
      return VK_SUCCESS;
   }

   if (!(dsync->vk.flags & VK_SYNC_IS_SHAREABLE)) {
      struct dzn_device *ddev = container_of(device, struct dzn_device, vk);
      ID3D12Fence *fence;
      if (FAILED(ID3D12Device1_CreateFence(ddev->dev, 0, D3D12_FENCE_FLAG_SHARED,
                                           &IID_ID3D12Fence, (void **)&fence)))
         return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      ID3D12Fence_Release(dsync->fence);
      dsync->fence = fence;
      dsync->vk.flags |= VK_SYNC_IS_SHAREABLE;
   }

   struct dzn_sync_file_waiter *waiter = calloc(1, sizeof(*waiter));
   if (!waiter)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   waiter->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
   waiter->cancel_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   waiter->device = container_of(device, struct dzn_device, vk);
   waiter->fence = dsync->fence;
   waiter->point = point;
   if (waiter->fd < 0 || waiter->cancel_fd < 0) {
      if (waiter->fd >= 0)
         close(waiter->fd);
      if (waiter->cancel_fd >= 0)
         close(waiter->cancel_fd);
      free(waiter);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   ID3D12Fence_AddRef(waiter->fence);

   if (thrd_create(&waiter->thread, dzn_sync_file_waiter_run, waiter) != thrd_success) {
      ID3D12Fence_Release(waiter->fence);
      close(waiter->fd);
      close(waiter->cancel_fd);
      free(waiter);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   struct dzn_device *ddev = waiter->device;
   mtx_lock(&ddev->sync_file_lock);
   list_for_each_entry_safe(struct dzn_sync_file_waiter, completed,
                            &ddev->sync_file_waiters, link) {
      if (p_atomic_read(&completed->done))
         dzn_sync_file_waiter_destroy(completed);
   }
   list_addtail(&waiter->link, &ddev->sync_file_waiters);
   mtx_unlock(&ddev->sync_file_lock);
   return VK_SUCCESS;
}

void
dzn_sync_binary_type_init(struct vk_sync_binary_type *type)
{
   type->sync.import_sync_file = dzn_sync_binary_import_sync_file;
   type->sync.export_sync_file = dzn_sync_binary_export_sync_file;
}
#endif

const struct vk_sync_type dzn_sync_type = {
   .size = sizeof(struct dzn_sync),
   .features = (enum vk_sync_features)
      (VK_SYNC_FEATURE_TIMELINE |
       VK_SYNC_FEATURE_GPU_WAIT |
       VK_SYNC_FEATURE_CPU_WAIT |
       VK_SYNC_FEATURE_CPU_SIGNAL |
       VK_SYNC_FEATURE_WAIT_ANY |
       VK_SYNC_FEATURE_WAIT_PENDING |
       VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL),

   .init = dzn_sync_init,
   .finish = dzn_sync_finish,
   .signal = dzn_sync_signal,
   .get_value = dzn_sync_get_value,
   .reset = dzn_sync_reset,
   .move = dzn_sync_move,
   .wait_many = dzn_sync_wait,
#ifdef _WIN32
   .import_win32_handle = dzn_sync_import_win32_handle,
   .export_win32_handle = dzn_sync_export_win32_handle,
   .set_win32_export_params = dzn_sync_prep_win32_export,
#else
   .import_opaque_fd = dzn_sync_import_opaque_fd,
   .export_opaque_fd = dzn_sync_export_opaque_fd,
#endif
};
