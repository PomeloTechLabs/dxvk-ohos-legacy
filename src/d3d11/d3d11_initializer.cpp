#include <cstring>

#include "d3d11_bc.h"
#include "d3d11_device.h"
#include "d3d11_initializer.h"
#include "../dxvk/dxvk_winehua_trace.h"
#include "../dxvk/dxvk_winehua_submit_stats.h"

namespace dxvk {

  D3D11Initializer::D3D11Initializer(
          D3D11Device*                pParent)
  : m_parent(pParent),
    m_device(pParent->GetDXVKDevice()),
    m_context(m_device->createContext()) {
    m_context->beginRecording(
      m_device->createCommandList());
  }

  
  D3D11Initializer::~D3D11Initializer() {

  }


  void D3D11Initializer::Flush() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_transferCommands != 0) {
      winehuaFlowTrace(str::format(
        "d3d11-init-flush commands=", m_transferCommands,
        " bytes=", m_transferMemory));
      FlushInternal();
    }
  }

  void D3D11Initializer::InitBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    winehuaFlowTrace(str::format(
      "d3d11-init-buffer size=", pBuffer->Desc()->ByteWidth,
      " usage=", uint32_t(pBuffer->Desc()->Usage),
      " bind=", pBuffer->Desc()->BindFlags,
      " initial=", pInitialData && pInitialData->pSysMem ? 1 : 0));

    VkMemoryPropertyFlags memFlags = pBuffer->GetBuffer()->memFlags();

    (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      ? InitHostVisibleBuffer(pBuffer, pInitialData)
      : InitDeviceLocalBuffer(pBuffer, pInitialData);
  }
  

  void D3D11Initializer::InitTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    const auto desc = pTexture->Desc();
    winehuaFlowTrace(str::format(
      "d3d11-init-texture size=", desc->Width, "x", desc->Height, "x", desc->Depth,
      " mips=", desc->MipLevels,
      " layers=", desc->ArraySize,
      " format=", uint32_t(desc->Format),
      " usage=", uint32_t(desc->Usage),
      " bind=", desc->BindFlags,
      " initial=", pInitialData && pInitialData->pSysMem ? 1 : 0));

    (pTexture->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_DIRECT)
      ? InitHostVisibleTexture(pTexture, pInitialData)
      : InitDeviceLocalTexture(pTexture, pInitialData);
  }


  void D3D11Initializer::InitUavCounter(
          D3D11UnorderedAccessView*   pUav) {
    auto counterBuffer = pUav->GetCounterSlice();

    if (!counterBuffer.defined())
      return;

    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_transferCommands += 1;

    const uint32_t zero = 0;
    m_context->updateBuffer(
      counterBuffer.buffer(),
      0, sizeof(zero), &zero);

    FlushImplicit();
  }


  void D3D11Initializer::InitDeviceLocalBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    DxvkBufferSlice bufferSlice = pBuffer->GetBufferSlice();

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      m_transferMemory   += bufferSlice.length();
      m_transferCommands += 1;
      
      m_context->uploadBuffer(
        bufferSlice.buffer(),
        pInitialData->pSysMem);
    } else {
      m_transferCommands += 1;

      m_context->initBuffer(
        bufferSlice.buffer());
    }

    FlushImplicit();
  }


  void D3D11Initializer::InitHostVisibleBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    // If the buffer is mapped, we can write data directly
    // to the mapped memory region instead of doing it on
    // the GPU. Same goes for zero-initialization.
    DxvkBufferSlice bufferSlice = pBuffer->GetBufferSlice();

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      std::memcpy(
        bufferSlice.mapPtr(0),
        pInitialData->pSysMem,
        bufferSlice.length());
    } else {
      std::memset(
        bufferSlice.mapPtr(0), 0,
        bufferSlice.length());
    }
    if (winehuaFlushDynamicMapped())
      m_context->flushMappedBuffer(
        bufferSlice.buffer(), bufferSlice.getSliceHandle());
  }


  void D3D11Initializer::InitDeviceLocalTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    
    Rc<DxvkImage> image = pTexture->GetImage();

    auto mapMode = pTexture->GetMapMode();
    auto desc = pTexture->Desc();

    VkFormat packedFormat = m_parent->LookupPackedFormat(desc->Format, pTexture->GetFormatMode()).Format;
    auto formatInfo = imageFormatInfo(packedFormat);

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      // pInitialData is an array that stores an entry for
      // every single subresource. Since we will define all
      // subresources, this counts as initialization.
      for (uint32_t layer = 0; layer < desc->ArraySize; layer++) {
        for (uint32_t level = 0; level < desc->MipLevels; level++) {
          const uint32_t id = D3D11CalcSubresource(
            level, layer, desc->MipLevels);

          VkOffset3D mipLevelOffset = { 0, 0, 0 };
          VkExtent3D mipLevelExtent = pTexture->MipLevelExtent(level);

          if (level == 0 && formatInfo->elementSize == 4
           && !formatInfo->flags.test(DxvkFormatFlag::BlockCompressed)
           && !formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
            winehuaTraceRgbaAlpha("init-src",
              pInitialData[id].pSysMem,
              mipLevelExtent.width, mipLevelExtent.height,
              pInitialData[id].SysMemPitch, uint32_t(packedFormat), id, layer);
          }

          if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING) {
            const void* uploadData = pInitialData[id].pSysMem;
            VkDeviceSize uploadRowPitch = pInitialData[id].SysMemPitch;
            VkDeviceSize uploadSlicePitch = pInitialData[id].SysMemSlicePitch;
            D3D11CpuImage converted;

            const bool bcEmulated = formatInfo->flags.test(DxvkFormatFlag::BlockCompressed)
                                 && image->info().format != packedFormat;
            const bool snormRtEmulated = pTexture->IsRgba8SnormRtEmulated();
            if (snormRtEmulated) {
              if (!ConvertD3D11Rgba8SnormToRgba16Float(
                    mipLevelExtent, uploadData, uploadRowPitch, uploadSlicePitch,
                    converted))
                throw DxvkError("WineHua: Failed to convert initial RGBA8 SNORM texture data");
              uploadData = converted.data.data();
              uploadRowPitch = converted.rowPitch;
              uploadSlicePitch = converted.slicePitch;
            } else if (bcEmulated) {
              /* The initial upload is where streaming transcodes land.  The
               * work is timed so a long frame can state how much of it was BC
               * decoding plus re-encoding rather than leaving it to guesswork. */
              const uint64_t winehuaTranscodeBeginUs = winehuaNowUs();
              const bool winehuaDecoded = DecodeD3D11BcImage(packedFormat,
                image->info().format, mipLevelExtent, uploadData,
                uploadRowPitch, uploadSlicePitch, converted);
              winehuaRecordTranscode(winehuaNowUs() - winehuaTranscodeBeginUs,
                uint64_t(mipLevelExtent.width) * uint64_t(mipLevelExtent.height) * 4ull);
              if (!winehuaDecoded)
                throw DxvkError("WineHua: Failed to decompress initial BC texture data");
              uploadData = converted.data.data();
              uploadRowPitch = converted.rowPitch;
              uploadSlicePitch = converted.slicePitch;
            }

            m_transferCommands += 1;
            m_transferMemory   += (snormRtEmulated || bcEmulated)
                                ? converted.data.size()
                                : pTexture->GetSubresourceLayout(formatInfo->aspectMask, id).Size;
            
            VkImageSubresourceLayers subresourceLayers;
            subresourceLayers.aspectMask     = formatInfo->aspectMask;
            subresourceLayers.mipLevel       = level;
            subresourceLayers.baseArrayLayer = layer;
            subresourceLayers.layerCount     = 1;
            
            if (formatInfo->aspectMask != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
              m_context->uploadImage(
                image, subresourceLayers,
                uploadData, uploadRowPitch, uploadSlicePitch);
            } else {
              m_context->updateDepthStencilImage(
                image, subresourceLayers,
                VkOffset2D { mipLevelOffset.x,     mipLevelOffset.y      },
                VkExtent2D { mipLevelExtent.width, mipLevelExtent.height },
                pInitialData[id].pSysMem,
                pInitialData[id].SysMemPitch,
                pInitialData[id].SysMemSlicePitch,
                packedFormat);
            }
          }

          if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_NONE) {
            auto mappedBuffer = pTexture->GetMappedBuffer(id);
            util::packImageData(mappedBuffer->mapPtr(0),
              pInitialData[id].pSysMem, pInitialData[id].SysMemPitch, pInitialData[id].SysMemSlicePitch,
              0, 0, pTexture->GetVkImageType(), mipLevelExtent, 1, formatInfo, formatInfo->aspectMask);
            if (level == 0 && formatInfo->elementSize == 4
             && !formatInfo->flags.test(DxvkFormatFlag::BlockCompressed)
             && !formatInfo->flags.test(DxvkFormatFlag::MultiPlane)) {
              winehuaTraceRgbaAlpha("init-packed",
                mappedBuffer->mapPtr(0),
                mipLevelExtent.width, mipLevelExtent.height,
                uint64_t(mipLevelExtent.width) * formatInfo->elementSize,
                uint32_t(packedFormat), id, layer);
            }
            if (winehuaFlushDynamicMapped()) {
              if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_STAGING)
                mappedBuffer->flushMappedSlice(mappedBuffer->getSliceHandle());
              else
                m_context->flushMappedBuffer(
                  mappedBuffer, mappedBuffer->getSliceHandle());
            }
          }
        }
      }
    } else {
      if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING) {
        m_transferCommands += 1;
        
        // While the Microsoft docs state that resource contents are
        // undefined if no initial data is provided, some applications
        // expect a resource to be pre-cleared.
        VkImageSubresourceRange subresources;
        subresources.aspectMask     = formatInfo->aspectMask;
        subresources.baseMipLevel   = 0;
        subresources.levelCount     = desc->MipLevels;
        subresources.baseArrayLayer = 0;
        subresources.layerCount     = desc->ArraySize;

        m_context->initImage(image, subresources, VK_IMAGE_LAYOUT_UNDEFINED);
      }

      if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_NONE) {
        for (uint32_t i = 0; i < pTexture->CountSubresources(); i++) {
          auto buffer = pTexture->GetMappedBuffer(i);
          std::memset(buffer->mapPtr(0), 0, buffer->info().size);
          if (winehuaFlushDynamicMapped()) {
            if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_STAGING)
              buffer->flushMappedSlice(buffer->getSliceHandle());
            else
              m_context->flushMappedBuffer(buffer, buffer->getSliceHandle());
          }
        }
      }
    }

    FlushImplicit();
  }


  void D3D11Initializer::InitHostVisibleTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    Rc<DxvkImage> image = pTexture->GetImage();

    for (uint32_t layer = 0; layer < image->info().numLayers; layer++) {
      for (uint32_t level = 0; level < image->info().mipLevels; level++) {
        VkImageSubresource subresource;
        subresource.aspectMask = image->formatInfo()->aspectMask;
        subresource.mipLevel   = level;
        subresource.arrayLayer = layer;

        VkExtent3D blockCount = util::computeBlockCount(
          image->mipLevelExtent(level),
          image->formatInfo()->blockSize);

        VkSubresourceLayout layout = image->querySubresourceLayout(subresource);

        auto initialData = pInitialData
          ? &pInitialData[D3D11CalcSubresource(level, layer, image->info().mipLevels)]
          : nullptr;

        for (uint32_t z = 0; z < blockCount.depth; z++) {
          for (uint32_t y = 0; y < blockCount.height; y++) {
            auto size = blockCount.width * image->formatInfo()->elementSize;
            auto dst = image->mapPtr(layout.offset + y * layout.rowPitch + z * layout.depthPitch);

            if (initialData) {
              auto src = reinterpret_cast<const char*>(initialData->pSysMem)
                       + y * initialData->SysMemPitch
                       + z * initialData->SysMemSlicePitch;
              std::memcpy(dst, src, size);
            } else {
              std::memset(dst, 0, size);
            }
          }
        }
      }
    }

    // Initialize the image on the GPU
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    VkImageSubresourceRange subresources = image->getAvailableSubresources();
    
    m_context->initImage(image, subresources, VK_IMAGE_LAYOUT_PREINITIALIZED);

    m_transferCommands += 1;
    FlushImplicit();
  }


  void D3D11Initializer::FlushImplicit() {
    if (m_transferCommands > MaxTransferCommands
     || m_transferMemory   > MaxTransferMemory)
      FlushInternal();
  }


  void D3D11Initializer::FlushInternal() {
    m_context->flushCommandList();
    
    m_transferCommands = 0;
    m_transferMemory   = 0;
  }

}
