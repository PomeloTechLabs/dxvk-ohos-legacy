#include "../d3d11/d3d11_options.h"
#include "../util/util_env.h"

#include "dxbc_options.h"

#include <cstring>
#include <string>

namespace dxvk {
  
  DxbcOptions::DxbcOptions() {

  }


  DxbcOptions::DxbcOptions(const Rc<DxvkDevice>& device, const D3D11Options& options) {
    const Rc<DxvkAdapter> adapter = device->adapter();

    const DxvkDeviceFeatures& devFeatures = device->features();
    const DxvkDeviceInfo& devInfo = adapter->devicePropertiesExt();

    useDepthClipWorkaround
      = !devFeatures.extDepthClipEnable.depthClipEnable;
    useStorageImageReadWithoutFormat
      = devFeatures.core.features.shaderStorageImageReadWithoutFormat;
    useSubgroupOpsForAtomicCounters
      = (devInfo.coreSubgroup.supportedStages     & VK_SHADER_STAGE_COMPUTE_BIT)
     && (devInfo.coreSubgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT);
    useDemoteToHelperInvocation
      = (devFeatures.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation);
    useSubgroupOpsForEarlyDiscard
      = (devInfo.coreSubgroup.subgroupSize >= 4)
     && (devInfo.coreSubgroup.supportedStages     & VK_SHADER_STAGE_FRAGMENT_BIT)
     && (devInfo.coreSubgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT);
    useSdivForBufferIndex
      = adapter->matchesDriver(DxvkGpuVendor::Nvidia, VK_DRIVER_ID_NVIDIA_PROPRIETARY_KHR, 0, 0);
    
    switch (device->config().useRawSsbo) {
      case Tristate::Auto:  minSsboAlignment = devInfo.core.properties.limits.minStorageBufferOffsetAlignment; break;
      case Tristate::True:  minSsboAlignment =  4u; break;
      case Tristate::False: minSsboAlignment = ~0u; break;
    }
    
    invariantPosition        = options.invariantPosition;
    enableRtOutputNanFixup   = options.enableRtOutputNanFixup;
    zeroInitWorkgroupMemory  = options.zeroInitWorkgroupMemory;
    forceTgsmBarriers        = options.forceTgsmBarriers;
    disableMsaa              = options.disableMsaa;
    useCombinedImageSampler  = env::getEnvVar("WINEHUA_DXVK_COMBINED_SAMPLER") == "1";
    emulateCustomBorderColor = !devFeatures.extCustomBorderColor
      .customBorderColorWithoutFormat
      && env::getEnvVar("WINEHUA_DXVK_DISABLE_CUSTOM_BORDER_EMULATION") != "1";
    {
      const std::string override = env::getEnvVar("WINEHUA_DXVK_PAD_CUBE_DREF_COORD");
      const std::string quirks = env::getEnvVar("WINEHUA_DXVK_QUIRKS");
      const char* deviceName = adapter->deviceProperties().deviceName;

      if (override == "1" || override == "true")
        padCubeDrefCoordinates = true;
      else if (override == "0" || override == "false")
        padCubeDrefCoordinates = false;
      else if (quirks.find("maleoon-cube-dref") != std::string::npos)
        padCubeDrefCoordinates = true;
      else if (quirks.find("no-maleoon-cube-dref") != std::string::npos)
        padCubeDrefCoordinates = false;
      else
        padCubeDrefCoordinates = std::strstr(deviceName, "Maleoon") != nullptr;
    }
    {
      const std::string override =
        env::getEnvVar("WINEHUA_DXVK_EMULATE_CUBE_ARRAY_DREF");
      const std::string quirks = env::getEnvVar("WINEHUA_DXVK_QUIRKS");
      const char* deviceName = adapter->deviceProperties().deviceName;

      if (override == "1" || override == "true")
        emulateCubeArrayDref = true;
      else if (override == "0" || override == "false")
        emulateCubeArrayDref = false;
      else if (quirks.find("maleoon-cube-array-dref") != std::string::npos)
        emulateCubeArrayDref = true;
      else if (quirks.find("no-maleoon-cube-array-dref") != std::string::npos)
        emulateCubeArrayDref = false;
      else
        emulateCubeArrayDref = std::strstr(deviceName, "Maleoon") != nullptr;
    }
    if (useCombinedImageSampler)
      Logger::info("WineHua: combined image sampler compatibility mode enabled");
    Logger::info(str::format(
      "WineHua: Cube Dref coordinate path=",
      padCubeDrefCoordinates ? "padded-vec4" : "native-minimal"));
    Logger::info(str::format(
      "WineHua: CubeArray Dref path=",
      emulateCubeArrayDref ? "2d-array-emulation" : "native"));
    Logger::info(str::format(
      "WineHua: custom border capability path=",
      emulateCustomBorderColor ? "shader-emulation" : "native",
      " customBorderColors=",
      devFeatures.extCustomBorderColor.customBorderColors ? 1 : 0,
      " customBorderColorWithoutFormat=",
      devFeatures.extCustomBorderColor.customBorderColorWithoutFormat ? 1 : 0));
    dynamicIndexedConstantBufferAsSsbo = options.constantBufferRangeCheck;

    // Disable subgroup early discard on Nvidia because it may hurt performance
    if (adapter->matchesDriver(DxvkGpuVendor::Nvidia, VK_DRIVER_ID_NVIDIA_PROPRIETARY_KHR, 0, 0))
      useSubgroupOpsForEarlyDiscard = false;
    
    // Figure out float control flags to match D3D11 rules
    if (options.floatControls) {
      if (devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat32)
        floatControl.set(DxbcFloatControlFlag::PreserveNan32);
      if (devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat64)
        floatControl.set(DxbcFloatControlFlag::PreserveNan64);

      if (devInfo.khrShaderFloatControls.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE) {
        if (devInfo.khrShaderFloatControls.shaderDenormFlushToZeroFloat32)
          floatControl.set(DxbcFloatControlFlag::DenormFlushToZero32);
        if (devInfo.khrShaderFloatControls.shaderDenormPreserveFloat64)
          floatControl.set(DxbcFloatControlFlag::DenormPreserve64);
      }
    }

    if (!devInfo.khrShaderFloatControls.shaderSignedZeroInfNanPreserveFloat32
     || adapter->matchesDriver(DxvkGpuVendor::Amd, VK_DRIVER_ID_MESA_RADV_KHR, 0, VK_MAKE_VERSION(20, 3, 0)))
      enableRtOutputNanFixup = true;
  }
  
}
