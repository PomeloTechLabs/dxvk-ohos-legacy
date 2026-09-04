#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "d3d11_bc.h"

/* Standalone MIT-licensed BC decoder copied into the native DXVK source.
 * This is decoder code only: it has no Wine runtime, ABI, loader, process,
 * Broker, or IPC dependency. WINE_UNUSED enables the BC6H/BC7 routines. */
#define BCDEC_STATIC
#define WINE_UNUSED 1
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#undef BCDEC_IMPLEMENTATION
#undef WINE_UNUSED
#undef BCDEC_STATIC

namespace dxvk {
  namespace {

    struct BcFormatInfo {
      uint32_t blockBytes = 0;
      uint32_t pixelBytes = 0;
      bool bc6h = false;
      bool bc6hSigned = false;
      bool signedRgtc = false;
      void (*decode)(const void*, void*, int) = nullptr;
    };

    BcFormatInfo GetBcFormatInfo(VkFormat format) {
      switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
          return { BCDEC_BC1_BLOCK_SIZE, 4, false, false, false, bcdec_bc1 };
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
          return { BCDEC_BC2_BLOCK_SIZE, 4, false, false, false, bcdec_bc2 };
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
          return { BCDEC_BC3_BLOCK_SIZE, 4, false, false, false, bcdec_bc3 };
        case VK_FORMAT_BC4_UNORM_BLOCK:
          return { BCDEC_BC4_BLOCK_SIZE, 1, false, false, false, bcdec_bc4 };
        case VK_FORMAT_BC4_SNORM_BLOCK:
          return { BCDEC_BC4_BLOCK_SIZE, 1, false, false, true, nullptr };
        case VK_FORMAT_BC5_UNORM_BLOCK:
          return { BCDEC_BC5_BLOCK_SIZE, 2, false, false, false, bcdec_bc5 };
        case VK_FORMAT_BC5_SNORM_BLOCK:
          return { BCDEC_BC5_BLOCK_SIZE, 2, false, false, true, nullptr };
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
          return { BCDEC_BC6H_BLOCK_SIZE, 8, true, false, false, nullptr };
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
          return { BCDEC_BC6H_BLOCK_SIZE, 8, true, true, false, nullptr };
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
          return { BCDEC_BC7_BLOCK_SIZE, 4, false, false, false, bcdec_bc7 };
        default:
          return { };
      }
    }

    int8_t ClampSnormEndpoint(int8_t value) {
      return value == INT8_MIN ? int8_t(-127) : value;
    }

    void DecodeSnormChannel(const uint8_t* source, int8_t* target,
                            uint32_t targetPitch, uint32_t pixelStride) {
      const int endpoint0 = ClampSnormEndpoint(int8_t(source[0]));
      const int endpoint1 = ClampSnormEndpoint(int8_t(source[1]));
      int values[8] = { endpoint0, endpoint1 };

      if (endpoint0 > endpoint1) {
        for (int i = 2; i < 8; i++)
          values[i] = (endpoint0 * (8 - i) + endpoint1 * (i - 1)) / 7;
      } else {
        for (int i = 2; i < 6; i++)
          values[i] = (endpoint0 * (6 - i) + endpoint1 * (i - 1)) / 5;
        values[6] = -127;
        values[7] = 127;
      }

      uint64_t indices = 0;
      for (uint32_t i = 0; i < 6; i++)
        indices |= uint64_t(source[2 + i]) << (8 * i);

      for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
          target[y * targetPitch + x * pixelStride]
            = int8_t(values[indices & 7]);
          indices >>= 3;
        }
      }
    }

    void DecodeSnormBlock(const uint8_t* source, uint8_t* target,
                          uint32_t pixelBytes) {
      DecodeSnormChannel(source, reinterpret_cast<int8_t*>(target),
                         4 * pixelBytes, pixelBytes);
      if (pixelBytes == 2)
        DecodeSnormChannel(source + 8, reinterpret_cast<int8_t*>(target + 1),
                           4 * pixelBytes, pixelBytes);
    }

    bool ComputeOutputSize(VkExtent3D extent, uint32_t pixelBytes,
                           VkDeviceSize& rowPitch, VkDeviceSize& slicePitch,
                           size_t& totalSize) {
      const uint64_t row = uint64_t(extent.width) * pixelBytes;
      const uint64_t slice = row * extent.height;
      const uint64_t total = slice * extent.depth;
      if (row > std::numeric_limits<VkDeviceSize>::max()
       || slice > std::numeric_limits<VkDeviceSize>::max()
       || total > std::numeric_limits<size_t>::max())
        return false;
      rowPitch = VkDeviceSize(row);
      slicePitch = VkDeviceSize(slice);
      totalSize = size_t(total);
      return true;
    }

  }

  bool DecodeD3D11BcImage(
          VkFormat                 format,
          VkExtent3D               extent,
    const void*                    source,
          VkDeviceSize             sourceRowPitch,
          VkDeviceSize             sourceSlicePitch,
          D3D11CpuImage&           result) {
    const BcFormatInfo info = GetBcFormatInfo(format);
    if (!info.blockBytes || !info.pixelBytes || !source
     || !extent.width || !extent.height || !extent.depth)
      return false;

    const uint32_t blocksX = (extent.width + 3) / 4;
    const uint32_t blocksY = (extent.height + 3) / 4;
    const VkDeviceSize minimumRowPitch = VkDeviceSize(blocksX) * info.blockBytes;
    if (!sourceRowPitch)
      sourceRowPitch = minimumRowPitch;
    if (sourceRowPitch < minimumRowPitch)
      return false;

    const VkDeviceSize minimumSlicePitch = sourceRowPitch * blocksY;
    if (!sourceSlicePitch)
      sourceSlicePitch = minimumSlicePitch;
    if (sourceSlicePitch < minimumSlicePitch)
      return false;

    size_t totalSize = 0;
    if (!ComputeOutputSize(extent, info.pixelBytes,
                           result.rowPitch, result.slicePitch, totalSize))
      return false;
    result.data.assign(totalSize, 0);

    const auto* sourceBytes = reinterpret_cast<const uint8_t*>(source);
    for (uint32_t z = 0; z < extent.depth; z++) {
      const uint8_t* sourceSlice = sourceBytes + z * sourceSlicePitch;
      uint8_t* targetSlice = result.data.data() + z * result.slicePitch;

      for (uint32_t blockY = 0; blockY < blocksY; blockY++) {
        for (uint32_t blockX = 0; blockX < blocksX; blockX++) {
          const uint8_t* compressed = sourceSlice
            + blockY * sourceRowPitch + blockX * info.blockBytes;
          alignas(4) uint8_t block[4 * 4 * 8] = { };

          if (info.bc6h) {
            alignas(4) uint16_t rgb[4 * 4 * 3] = { };
            /* BCDEC's BC6H routine uses a destination pitch measured in
             * uint16_t elements (the other decoders use bytes). */
            bcdec_bc6h_half(compressed, rgb, 4 * 3,
                            info.bc6hSigned ? 1 : 0);
            auto* rgba = reinterpret_cast<uint16_t*>(block);
            for (uint32_t i = 0; i < 16; i++) {
              rgba[i * 4 + 0] = rgb[i * 3 + 0];
              rgba[i * 4 + 1] = rgb[i * 3 + 1];
              rgba[i * 4 + 2] = rgb[i * 3 + 2];
              rgba[i * 4 + 3] = 0x3c00; // half-float 1.0
            }
          } else if (info.signedRgtc) {
            DecodeSnormBlock(compressed, block, info.pixelBytes);
          } else {
            info.decode(compressed, block, 4 * info.pixelBytes);
          }

          const uint32_t copyWidth = std::min(4u, extent.width - blockX * 4);
          const uint32_t copyHeight = std::min(4u, extent.height - blockY * 4);
          for (uint32_t y = 0; y < copyHeight; y++) {
            std::memcpy(targetSlice
                          + (blockY * 4 + y) * result.rowPitch
                          + blockX * 4 * info.pixelBytes,
                        block + y * 4 * info.pixelBytes,
                        copyWidth * info.pixelBytes);
          }
        }
      }
    }

    return true;
  }

}
