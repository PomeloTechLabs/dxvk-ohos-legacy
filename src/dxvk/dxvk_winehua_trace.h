#pragma once

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>

#include "../util/log/log.h"

namespace dxvk {

  inline bool winehuaAlphaTraceEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_TRACE_ALPHA");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  inline void winehuaTraceRgbaAlpha(
    const char* stage,
    const void* data,
          uint32_t width,
          uint32_t height,
          uint64_t rowPitch,
          uint32_t format,
          uint32_t subresource,
          uint32_t layer) {
    if (!winehuaAlphaTraceEnabled() || !data
     || width < 128 || height < 128 || rowPitch < uint64_t(width) * 4)
      return;

    static std::atomic<uint32_t> emitted { 0 };
    const uint32_t index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index >= 256) {
      if (index == 256)
        Logger::info("WineHuaAlpha: further records suppressed");
      return;
    }

    uint64_t alphaZero = 0;
    uint64_t alphaFull = 0;
    uint64_t alphaOther = 0;
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);

    for (uint32_t y = 0; y < height; y++) {
      const auto* row = bytes + uint64_t(y) * rowPitch;
      for (uint32_t x = 0; x < width; x++) {
        const auto* pixel = row + uint64_t(x) * 4;
        for (uint32_t channel = 0; channel < 4; channel++) {
          hash ^= pixel[channel];
          hash *= 1099511628211ull;
        }

        if (pixel[3] == 0)
          alphaZero++;
        else if (pixel[3] == 255)
          alphaFull++;
        else
          alphaOther++;
      }
    }

    Logger::info(std::string("WineHuaAlpha: stage=") + stage
      + " size=" + std::to_string(width) + "x" + std::to_string(height)
      + " pitch=" + std::to_string(rowPitch)
      + " format=" + std::to_string(format)
      + " subresource=" + std::to_string(subresource)
      + " layer=" + std::to_string(layer)
      + " alpha0=" + std::to_string(alphaZero)
      + " alpha255=" + std::to_string(alphaFull)
      + " alphaOther=" + std::to_string(alphaOther)
      + " hash=" + std::to_string(hash));
  }

  enum class WineHuaDualSrcMode {
    TwoPass,
    SecondaryReplace,
    SecondaryMultiply,
    PrimaryReplace,
    PrimaryAdd,
  };

  inline WineHuaDualSrcMode winehuaDualSrcMode() {
    const char* value = std::getenv("DXVK_WINEHUA_DUAL_SRC_MODE");

    if (value && !std::strcmp(value, "secondary-replace"))
      return WineHuaDualSrcMode::SecondaryReplace;
    if (value && !std::strcmp(value, "secondary-multiply"))
      return WineHuaDualSrcMode::SecondaryMultiply;
    if (value && !std::strcmp(value, "primary-replace"))
      return WineHuaDualSrcMode::PrimaryReplace;
    if (value && !std::strcmp(value, "primary-add"))
      return WineHuaDualSrcMode::PrimaryAdd;
    return WineHuaDualSrcMode::TwoPass;
  }

  inline const char* winehuaDualSrcModeName(WineHuaDualSrcMode mode) {
    switch (mode) {
      case WineHuaDualSrcMode::SecondaryReplace:  return "secondary-replace";
      case WineHuaDualSrcMode::SecondaryMultiply: return "secondary-multiply";
      case WineHuaDualSrcMode::PrimaryReplace:    return "primary-replace";
      case WineHuaDualSrcMode::PrimaryAdd:        return "primary-add";
      default:                                    return "two-pass";
    }
  }

  /* Narrow, opt-in diagnostics for the WineHua sampled-image investigation.
   * The normal DXVK runtime never emits these records. */
  inline bool winehuaSampleTraceEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_TRACE_SAMPLED");
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline void winehuaSampleTraceEmit(const std::string& message) {
    Logger::info("WineHuaSampled: " + message);
  }

  inline bool winehuaRenderPassTraceAllow() {
    if (!winehuaSampleTraceEnabled())
      return false;

    static std::atomic<uint32_t> emitted { 0 };
    const uint32_t index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index < 1024)
      return true;
    if (index == 1024)
      Logger::info("WineHuaRenderPass: further records suppressed");
    return false;
  }

  inline void winehuaRenderPassTraceEmit(const std::string& message) {
    Logger::info("WineHuaRenderPass: " + message);
  }

  /* Function arguments are evaluated before entering an inline helper.  Keep
   * the enable/limit check at the call site so disabled diagnostics do not
   * build formatted strings in draw, barrier, or resource hot paths. */
#define winehuaSampleTrace(message)                                             \
  do {                                                                          \
    if (winehuaSampleTraceEnabled())                                             \
      winehuaSampleTraceEmit((message));                                         \
  } while (false)

#define winehuaRenderPassTrace(message)                                         \
  do {                                                                          \
    if (winehuaRenderPassTraceAllow())                                           \
      winehuaRenderPassTraceEmit((message));                                     \
  } while (false)

  /* Render-target capture is deliberately separate from the normal sampled
   * trace. It is enabled for one selected frame only and must never become a
   * product rendering path. */
  inline bool winehuaRenderTargetDumpEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_RT");
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline uint64_t winehuaRenderTargetDumpFrame() {
    static uint64_t frame = UINT64_MAX;
    if (frame == UINT64_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_FRAME");
      char* end = nullptr;
      frame = value && value[0]
        ? std::strtoull(value, &end, 10) : 0;
      if (!end || *end != '\0')
        frame = 0;
    }
    return frame;
  }

  inline bool winehuaDrawTraceEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
      const char* value = std::getenv("WINEHUA_DXVK_TRACE_DRAWS");
      enabled = value && value[0] == '1' ? 1 : 0;
    }
    return enabled != 0;
  }

  inline bool winehuaCameraTraceEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("WINEHUA_DXVK_TRACE_CAMERA");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  inline bool winehuaPresentImageTraceEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("WINEHUA_DXVK_TRACE_PRESENT_IMAGE");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  inline uint32_t winehuaDrawTracePass() {
    static uint32_t pass = UINT32_MAX;
    static bool initialized = false;
    if (!initialized) {
      const char* value = std::getenv("WINEHUA_DXVK_TRACE_PASS");
      char* end = nullptr;
      if (value && value[0]) {
        const unsigned long parsed = std::strtoul(value, &end, 10);
        pass = end && *end == '\0' ? uint32_t(parsed) : UINT32_MAX;
      }
      initialized = true;
    }
    return pass;
  }

  inline uint32_t winehuaDrawTraceSecondPass() {
    static uint32_t pass = UINT32_MAX;
    static bool initialized = false;
    if (!initialized) {
      const char* value = std::getenv("WINEHUA_DXVK_TRACE_PASS_SECOND");
      char* end = nullptr;
      if (value && value[0]) {
        const unsigned long parsed = std::strtoul(value, &end, 10);
        pass = end && *end == '\0' ? uint32_t(parsed) : UINT32_MAX;
      }
      initialized = true;
    }
    return pass;
  }

  inline uint32_t winehuaDrawTraceMaxDraws() {
    static uint32_t count = UINT32_MAX;
    if (count == UINT32_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_TRACE_DRAW_MAX");
      char* end = nullptr;
      count = value && value[0]
        ? uint32_t(std::strtoul(value, &end, 10)) : 2048u;
      if (!end || *end != '\0' || !count)
        count = 2048u;
    }
    return count;
  }

  inline uint32_t winehuaRenderTargetDumpDraw() {
    static uint32_t draw = UINT32_MAX;
    static bool initialized = false;
    if (!initialized) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_DRAW");
      char* end = nullptr;
      if (value && value[0]) {
        const unsigned long parsed = std::strtoul(value, &end, 10);
        draw = end && *end == '\0' ? uint32_t(parsed) : UINT32_MAX;
      }
      initialized = true;
    }
    return draw;
  }

  inline const char* winehuaRenderTargetDumpFragmentShader() {
    const char* value = std::getenv("WINEHUA_DXVK_DUMP_FS");
    return value && value[0] ? value : "";
  }

  inline uint64_t winehuaRenderTargetDumpIndexCount() {
    static uint64_t count = UINT64_MAX;
    if (count == UINT64_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_INDEX_COUNT");
      char* end = nullptr;
      if (value && value[0]) {
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        count = end && *end == '\0' ? parsed : UINT64_MAX;
      }
    }
    return count;
  }

  inline uint64_t winehuaRenderTargetDumpFirstIndex() {
    static uint64_t first = UINT64_MAX;
    if (first == UINT64_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_FIRST_INDEX");
      char* end = nullptr;
      if (value && value[0]) {
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        first = end && *end == '\0' ? parsed : UINT64_MAX;
      }
    }
    return first;
  }

  inline int64_t winehuaRenderTargetDumpVertexOffset() {
    static int64_t offset = INT64_MIN;
    static bool initialized = false;
    if (!initialized) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_VERTEX_OFFSET");
      char* end = nullptr;
      if (value && value[0]) {
        const long long parsed = std::strtoll(value, &end, 10);
        offset = end && *end == '\0' ? parsed : INT64_MIN;
      }
      initialized = true;
    }
    return offset;
  }

  inline bool winehuaTargetDrawCaptureEnabled() {
    return winehuaDrawTraceEnabled()
        && winehuaRenderTargetDumpEnabled()
        && (winehuaRenderTargetDumpDraw() != UINT32_MAX
         || winehuaRenderTargetDumpFragmentShader()[0]);
  }

  inline uint32_t winehuaRenderTargetDumpMaxAttachments() {
    static uint32_t count = UINT32_MAX;
    if (count == UINT32_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_RT_MAX");
      char* end = nullptr;
      count = value && value[0]
        ? uint32_t(std::strtoul(value, &end, 10)) : 12u;
      if (!end || *end != '\0' || !count)
        count = 12u;
    }
    return count;
  }

  inline bool winehuaRenderTargetDumpSampledEnabled() {
    const char* value = std::getenv("WINEHUA_DXVK_DUMP_SAMPLED");
    return !value || value[0] != '0';
  }

  inline uint32_t winehuaRenderTargetDumpFirstPass() {
    static uint32_t pass = UINT32_MAX;
    if (pass == UINT32_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_PASS_START");
      char* end = nullptr;
      pass = value && value[0]
        ? uint32_t(std::strtoul(value, &end, 10)) : 0u;
      if (!end || *end != '\0')
        pass = 0u;
    }
    return pass;
  }

  inline uint64_t winehuaRenderTargetDumpMaxBytes() {
    static uint64_t bytes = UINT64_MAX;
    if (bytes == UINT64_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_RT_MAX_BYTES");
      char* end = nullptr;
      bytes = value && value[0]
        ? std::strtoull(value, &end, 10) : (64ull << 20);
      if (!end || *end != '\0' || !bytes)
        bytes = 64ull << 20;
    }
    return bytes;
  }

  inline uint64_t winehuaGeometryDumpMaxBytes() {
    static uint64_t bytes = UINT64_MAX;
    if (bytes == UINT64_MAX) {
      const char* value = std::getenv("WINEHUA_DXVK_DUMP_GEOMETRY_MAX_BYTES");
      char* end = nullptr;
      bytes = value && value[0]
        ? std::strtoull(value, &end, 10) : (32ull << 20);
      if (!end || *end != '\0' || !bytes)
        bytes = 32ull << 20;
    }
    return bytes;
  }

  inline const char* winehuaRenderTargetDumpPath() {
    const char* value = std::getenv("WINEHUA_DXVK_DUMP_RT_PATH");
    return value && value[0] ? value : ".";
  }

  inline bool winehuaForceSampledGeneral() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_FORCE_SAMPLED_GENERAL");
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline bool winehuaCommandQueryReset() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_COMMAND_QUERY_RESET");
      #if defined(DXVK_NATIVE_OHOS)
      if (!value) return true;
      #endif
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline bool winehuaQueryTraceEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_TRACE_QUERY");
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline bool winehuaQueryTraceAllow() {
    if (!winehuaQueryTraceEnabled())
      return false;

    static std::atomic<uint32_t> emitted { 0 };
    const uint32_t index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index < 512)
      return true;
    if (index == 512)
      Logger::info("WineHuaQuery: further records suppressed");
    return false;
  }

  inline void winehuaQueryTraceEmit(const std::string& message) {
    Logger::info("WineHuaQuery: " + message);
  }

  /* Bounded startup-flow diagnostics. These are deliberately opt-in because
   * shader and pipeline creation can happen on multiple worker threads. */
  inline bool winehuaFlowTraceEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_TRACE_FLOW");
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline bool winehuaFlowTraceAllow() {
    if (!winehuaFlowTraceEnabled())
      return false;

    static std::atomic<uint32_t> emitted { 0 };
    const uint32_t index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index < 4096)
      return true;
    if (index == 4096)
      Logger::info("WineHuaFlow: further records suppressed");
    return false;
  }

  inline void winehuaFlowTraceEmit(const std::string& message) {
    Logger::info("WineHuaFlow: " + message);
  }

#define winehuaQueryTrace(message)                                              \
  do {                                                                          \
    if (winehuaQueryTraceAllow())                                               \
      winehuaQueryTraceEmit((message));                                         \
  } while (false)

#define winehuaFlowTrace(message)                                               \
  do {                                                                          \
    if (winehuaFlowTraceAllow())                                                \
      winehuaFlowTraceEmit((message));                                          \
  } while (false)

  inline bool winehuaFlushDynamicMapped() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED");
      #if defined(DXVK_NATIVE_OHOS)
      if (!value) return true;
      #endif
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline bool winehuaBatchMappedFlush() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_BATCH_MAPPED_FLUSH");
      #if defined(DXVK_NATIVE_OHOS)
      if (!value) return true;
      #endif
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  inline bool winehuaBatchMappedFlushStats() {
    static const bool enabled = [] {
      const char* value = std::getenv(
        "DXVK_WINEHUA_BATCH_MAPPED_FLUSH_STATS");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  inline bool winehuaPreciseShadowEnabled() {
    #if defined(DXVK_NATIVE_OHOS)
    // In-process Vulkan: invalidate is GPU-to-CPU visibility, never a broker
    // CPU-write marker. Do not allow a guest-shadow environment override.
    return true;
    #else
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_PRECISE_SHADOW");
      return value && value[0] == '1';
    }();
    return enabled;
    #endif
  }

  inline bool winehuaFifoBufferSlices() {
    static const bool enabled = [] {
      const char* value = std::getenv("DXVK_WINEHUA_FIFO_BUFFER_SLICES");
      return value && value[0] == '1';
    }();
    return enabled;
  }

  inline bool winehuaForceHeavenPass2DepthAlways() {
    static const bool enabled = [] {
      const char* value = std::getenv(
        "WINEHUA_DXVK_FORCE_HEAVEN_PASS2_DEPTH_ALWAYS");
      return value && value[0] == '1';
    }();
    return enabled;
  }

}
