/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/backends/vulkan/runtime/vk_api/Runtime.h>

#include <executorch/backends/vulkan/runtime/vk_api/Adapter.h>

#include <cstring>
#include <iostream>
#include <sstream>

#ifdef USE_VOLK_HEADER_ONLY
// For volk.h, define this before including volk.h in exactly one CPP file.
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif /* USE_VOLK_HEADER_ONLY */

namespace vkcompute {
namespace vkapi {

#define PRINT_CASE(name)       \
  case MemoryAccessType::name: \
    out << #name;              \
    break;

std::ostream& operator<<(std::ostream& out, const MemoryAccessType& tag) {
  switch (tag) {
    PRINT_CASE(NONE)
    PRINT_CASE(READ)
    PRINT_CASE(WRITE)
  }
  return out;
}

#undef PRINT_CASE

namespace {

void find_requested_layers_and_extensions(
    std::vector<const char*>& enabled_layers,
    std::vector<const char*>& enabled_extensions,
    const std::vector<const char*>& requested_layers,
    const std::vector<const char*>& requested_extensions) {
  // Get supported instance layers
  uint32_t layer_count = 0;
  VK_CHECK(vkEnumerateInstanceLayerProperties(&layer_count, nullptr));

  std::vector<VkLayerProperties> layer_properties(layer_count);
  VK_CHECK(vkEnumerateInstanceLayerProperties(
      &layer_count, layer_properties.data()));

  // Search for requested layers
  for (const auto& requested_layer : requested_layers) {
    for (const auto& layer : layer_properties) {
      if (strcmp(requested_layer, layer.layerName) == 0) {
        enabled_layers.push_back(requested_layer);
        break;
      }
    }
  }

  // Get supported instance extensions
  uint32_t extension_count = 0;
  VK_CHECK(vkEnumerateInstanceExtensionProperties(
      nullptr, &extension_count, nullptr));

  std::vector<VkExtensionProperties> extension_properties(extension_count);
  VK_CHECK(vkEnumerateInstanceExtensionProperties(
      nullptr, &extension_count, extension_properties.data()));

  // Search for requested extensions
  for (const auto& requested_extension : requested_extensions) {
    for (const auto& extension : extension_properties) {
      if (strcmp(requested_extension, extension.extensionName) == 0) {
        enabled_extensions.push_back(requested_extension);
        break;
      }
    }
  }
}

VkInstance create_instance(const RuntimeConfig& config) {
  const VkApplicationInfo application_info{
      VK_STRUCTURE_TYPE_APPLICATION_INFO, // sType
      nullptr, // pNext
      "PyTorch Vulkan Backend", // pApplicationName
      0, // applicationVersion
      nullptr, // pEngineName
      0, // engineVersion
      VK_API_VERSION_1_1, // apiVersion
  };

  std::vector<const char*> enabled_layers;
  std::vector<const char*> enabled_extensions;

  std::vector<const char*> requested_layers;
  std::vector<const char*> requested_extensions;

  if (config.enable_validation_messages) {
    requested_layers.emplace_back("VK_LAYER_KHRONOS_validation");
#ifdef VK_EXT_debug_report
    requested_extensions.emplace_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
#endif /* VK_EXT_debug_report */
  }

  VkInstanceCreateFlags instance_flags = 0;
#ifdef __APPLE__
  instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  requested_extensions.emplace_back(
      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

  find_requested_layers_and_extensions(
      enabled_layers,
      enabled_extensions,
      requested_layers,
      requested_extensions);

  const void* instance_create_next = nullptr;
  // VkConfig on Mac platforms does not expose debugPrintf settings for whatever
  // reason so it has to be enabled manually.
#if defined(__APPLE__) && defined(VULKAN_DEBUG)
  std::vector<VkValidationFeatureEnableEXT> enabled_validation_features{
      VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
  };
  VkValidationFeaturesEXT validation_features = {
      VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, // sType
      nullptr, // pNext
      static_cast<uint32_t>(
          enabled_validation_features.size()), // enabledValidationFeatureCount
      enabled_validation_features.data(), // pEnabledValidationFeatures
      0,
      nullptr, // pDisabledValidationFeatures
  };
  instance_create_next = &validation_features;
#endif /* __APPLE__ && VULKAN_DEBUG */

  const VkInstanceCreateInfo instance_create_info{
      VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, // sType
      instance_create_next, // pNext
      instance_flags, // flags
      &application_info, // pApplicationInfo
      static_cast<uint32_t>(enabled_layers.size()), // enabledLayerCount
      enabled_layers.data(), // ppEnabledLayerNames
      static_cast<uint32_t>(enabled_extensions.size()), // enabledExtensionCount
      enabled_extensions.data(), // ppEnabledExtensionNames
  };

  VkInstance instance{};
  VK_CHECK(vkCreateInstance(&instance_create_info, nullptr, &instance));
  VK_CHECK_COND(instance, "Invalid Vulkan instance!");

#ifdef USE_VULKAN_VOLK
  volkLoadInstance(instance);
#endif /* USE_VULKAN_VOLK */

  return instance;
}

std::vector<Runtime::DeviceMapping> create_physical_devices(
    VkInstance instance) {
  if (instance == VK_NULL_HANDLE) {
    return std::vector<Runtime::DeviceMapping>();
  }

  uint32_t device_count = 0;
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, nullptr));

  std::vector<VkPhysicalDevice> devices(device_count);
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()));

  std::vector<Runtime::DeviceMapping> device_mappings;
  device_mappings.reserve(device_count);
  for (VkPhysicalDevice physical_device : devices) {
    device_mappings.emplace_back(PhysicalDevice(physical_device), -1);
  }

  return device_mappings;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_report_callback_fn(
    const VkDebugReportFlagsEXT flags,
    const VkDebugReportObjectTypeEXT /* object_type */,
    const uint64_t /* object */,
    const size_t /* location */,
    const int32_t message_code,
    const char* const layer_prefix,
    const char* const message,
    void* const /* user_data */) {
  (void)flags;

  std::stringstream stream;
  stream << layer_prefix << " " << message_code << " " << message << std::endl;
  const std::string log = stream.str();

  std::cout << log;

  return VK_FALSE;
}

VkDebugReportCallbackEXT create_debug_report_callback(
    VkInstance instance,
    const RuntimeConfig config) {
  if (instance == VK_NULL_HANDLE || !config.enable_validation_messages) {
    return VkDebugReportCallbackEXT{};
  }

  const VkDebugReportCallbackCreateInfoEXT debugReportCallbackCreateInfo{
      VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT, // sType
      nullptr, // pNext
      VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
          VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
          VK_DEBUG_REPORT_ERROR_BIT_EXT |
          VK_DEBUG_REPORT_DEBUG_BIT_EXT, // flags
      debug_report_callback_fn, // pfnCallback
      nullptr, // pUserData
  };

  const auto vkCreateDebugReportCallbackEXT =
      (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(
          instance, "vkCreateDebugReportCallbackEXT");

  VK_CHECK_COND(
      vkCreateDebugReportCallbackEXT,
      "Could not load vkCreateDebugReportCallbackEXT");

  VkDebugReportCallbackEXT debug_report_callback{};
  VK_CHECK(vkCreateDebugReportCallbackEXT(
      instance,
      &debugReportCallbackCreateInfo,
      nullptr,
      &debug_report_callback));

  VK_CHECK_COND(debug_report_callback, "Invalid Vulkan debug report callback!");

  return debug_report_callback;
}

//
// Adapter selection methods
//

uint32_t select_first(const std::vector<Runtime::DeviceMapping>& devices) {
  if (devices.empty()) {
    return devices.size() + 1; // return out of range to signal invalidity
  }

  // Select the first adapter that has compute capability
  for (size_t i = 0; i < devices.size(); ++i) {
    if (devices[i].first.num_compute_queues > 0) {
      return i;
    }
  }

  return devices.size() + 1;
}

//
// Global runtime initialization
//

std::unique_ptr<Runtime> init_global_vulkan_runtime(
    const std::string& cache_data_path) {
  // Load Vulkan drivers
#if defined(USE_VULKAN_VOLK)
  if (VK_SUCCESS != volkInitialize()) {
    return std::unique_ptr<Runtime>(nullptr);
  }
#elif defined(USE_VULKAN_WRAPPER)
  if (!InitVulkan()) {
    return std::unique_ptr<Runtime>(nullptr);
  }
#endif /* USE_VULKAN_VOLK, USE_VULKAN_WRAPPER */

  const bool enable_validation_messages =
#if defined(VULKAN_DEBUG)
      true;
#else
      false;
#endif /* VULKAN_DEBUG */
  const bool init_default_device = true;
  const uint32_t num_requested_queues = 1; // TODO: raise this value

  const RuntimeConfig default_config{
      enable_validation_messages,
      init_default_device,
      AdapterSelector::First,
      num_requested_queues,
      cache_data_path,
  };

  try {
    return std::make_unique<Runtime>(default_config);
  } catch (...) {
  }

  return std::unique_ptr<Runtime>(nullptr);
}

} // namespace

Runtime::Runtime(const RuntimeConfig config)
    : config_(config),
      instance_(create_instance(config_)),
      device_mappings_(create_physical_devices(instance_)),
      adapters_{},
      default_adapter_i_(UINT32_MAX),
      debug_report_callback_(create_debug_report_callback(instance_, config_)) {
  // List of adapters will never exceed the number of physical devices
  adapters_.reserve(device_mappings_.size());

  if (config.init_default_device) {
    try {
      switch (config.default_selector) {
        case AdapterSelector::First:
          default_adapter_i_ = create_adapter(select_first);
      }
    } catch (...) {
    }
  }
}

Runtime::~Runtime() {
  if (instance_ == VK_NULL_HANDLE) {
    return;
  }

  // Clear adapters list to trigger device destruction before destroying
  // VkInstance
  adapters_.clear();

  // Instance must be destroyed last as its used to destroy the debug report
  // callback.
  if (debug_report_callback_) {
    const auto vkDestroyDebugReportCallbackEXT =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
            instance_, "vkDestroyDebugReportCallbackEXT");

    if (vkDestroyDebugReportCallbackEXT) {
      vkDestroyDebugReportCallbackEXT(
          instance_, debug_report_callback_, nullptr);
    }

    debug_report_callback_ = {};
  }

  vkDestroyInstance(instance_, nullptr);
  instance_ = VK_NULL_HANDLE;
}

uint32_t Runtime::create_adapter(const Selector& selector) {
  VK_CHECK_COND(
      !device_mappings_.empty(),
      "Pytorch Vulkan Runtime: Could not initialize adapter because no "
      "devices were found by the Vulkan instance.");

  uint32_t physical_device_i = selector(device_mappings_);
  VK_CHECK_COND(
      physical_device_i < device_mappings_.size(),
      "Pytorch Vulkan Runtime: no suitable device adapter was selected! "
      "Device could not be initialized");

  Runtime::DeviceMapping& device_mapping = device_mappings_[physical_device_i];
  // If an Adapter has already been created, return that
  int32_t adapter_i = device_mapping.second;
  if (adapter_i >= 0) {
    return adapter_i;
  }
  // Otherwise, create an adapter for the selected physical device
  adapter_i = utils::safe_downcast<int32_t>(adapters_.size());
  adapters_.emplace_back(new Adapter(
      instance_,
      device_mapping.first,
      config_.num_requested_queues,
      config_.cache_data_path));
  device_mapping.second = adapter_i;

  return adapter_i;
}

std::string& set_and_get_pipeline_cache_data_path(
    const std::string& file_path) {
  // The global cache data path is declared as a static local variable for the
  // same reasons as the global runtime below.
#if defined(ETVK_DEFAULT_CACHE_PATH)
  static std::string global_cache_data_path = ETVK_DEFAULT_CACHE_PATH;
#else
  static std::string global_cache_data_path;
#endif /* ETVK_DEFAULT_CACHE_PATH */

  if (file_path.size() > 0) {
    global_cache_data_path = file_path;
  }
  return global_cache_data_path;
}

Runtime* runtime() {
  // The global vulkan runtime is declared as a static local variable within a
  // non-static function to ensure it has external linkage. If it were a global
  // static variable there would be one copy per translation unit that includes
  // Runtime.h as it would have internal linkage.
  static const std::unique_ptr<Runtime> p_runtime =
      init_global_vulkan_runtime(set_and_get_pipeline_cache_data_path(""));

  VK_CHECK_COND(
      p_runtime,
      "Pytorch Vulkan Runtime: The global runtime could not be retrieved "
      "because it failed to initialize.");

  return p_runtime.get();
}

std::unique_ptr<Adapter> init_external_adapter(
    const VkInstance instance,
    const VkPhysicalDevice physical_device,
    const VkDevice logical_device,
    const uint32_t num_queues,
    const std::string& cache_data_path) {
  if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE ||
      logical_device == VK_NULL_HANDLE) {
    return std::unique_ptr<Adapter>(nullptr);
  }

  return std::make_unique<Adapter>(
      instance, physical_device, logical_device, num_queues, cache_data_path);
}

Adapter* set_and_get_external_adapter(
    const VkInstance instance,
    const VkPhysicalDevice physical_device,
    const VkDevice logical_device) {
  static const std::unique_ptr<Adapter> p_external_adapter =
      init_external_adapter(
          instance,
          physical_device,
          logical_device,
          1,
          set_and_get_pipeline_cache_data_path(""));

  return p_external_adapter.get();
}

} // namespace vkapi
} // namespace vkcompute
