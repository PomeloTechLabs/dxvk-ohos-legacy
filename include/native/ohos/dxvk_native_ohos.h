#ifndef DXVK_NATIVE_OHOS_H
#define DXVK_NATIVE_OHOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NativeWindow OHNativeWindow;
typedef uint64_t DXVKOhosWindowHandle;

enum DXVKOhosWindowStatus {
  DXVK_OHOS_WINDOW_OK = 0,
  DXVK_OHOS_WINDOW_INVALID_ARGUMENT = -1,
  DXVK_OHOS_WINDOW_UNAVAILABLE = -2,
  DXVK_OHOS_WINDOW_OUT_OF_MEMORY = -3
};

typedef struct DXVKOhosWindowInfo {
  uint32_t width;
  uint32_t height;
  uint64_t revision;
} DXVKOhosWindowInfo;

#define DXVK_OHOS_API __attribute__((visibility("default")))

/* Register during the XComponent surface-created callback, while window is
 * valid. DXVK retains one native-object reference. The returned value is an
 * opaque, non-reused ID, not an OHNativeWindow pointer. On ARM64 it will be
 * passed as the native DXGI OutputWindow/ HWND value by the WSI frontend.
 * A native window may have only one live registration.
 */
DXVK_OHOS_API int32_t DXVKOhosRegisterWindow(
  OHNativeWindow* window, uint32_t width, uint32_t height,
  DXVKOhosWindowHandle* handle);

/* Call from surface-changed. Zero width or height suspends presentation.
 * A size change advances revision and requires swapchain recreation.
 */
DXVK_OHOS_API int32_t DXVKOhosResizeWindow(
  DXVKOhosWindowHandle handle, uint32_t width, uint32_t height);

/* Call before returning from surface-destroyed. Retires the ID and waits for
 * any in-progress WSI lease to finish. Subsequent lookups/operations fail.
 * The native reference remains held until existing Presenter owners release
 * their surfaces. A newly created surface must get a new registration.
 * Lifecycle functions must not be called from inside a WSI lease.
 */
DXVK_OHOS_API int32_t DXVKOhosUnregisterWindow(DXVKOhosWindowHandle handle);

DXVK_OHOS_API int32_t DXVKOhosGetWindowInfo(
  DXVKOhosWindowHandle handle, DXVKOhosWindowInfo* info);

#ifdef __cplusplus
}
#endif

#endif
