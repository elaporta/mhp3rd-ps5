// SPDX-License-Identifier: GPL-3.0-or-later
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "probe.hpp"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include "ge_gpu.hpp"
#if YAKUMO_WITH_INTEGRATION
#include "integration_markers.hpp"
#endif

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance, const char*);
namespace yakumo::ps5 {
namespace {
#define INSTANCE_COMMANDS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceFormatProperties) \
    X(GetPhysicalDeviceDisplayPropertiesKHR) X(GetDisplayModePropertiesKHR) \
    X(GetPhysicalDeviceDisplayPlanePropertiesKHR) X(GetDisplayPlaneSupportedDisplaysKHR) \
    X(GetDisplayPlaneCapabilitiesKHR) X(CreateDisplayPlaneSurfaceKHR) X(DestroySurfaceKHR) \
    X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_COMMANDS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) X(CreateImageView) X(DestroyImageView) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
    X(AcquireNextImageKHR) X(QueueSubmit) X(QueuePresentKHR) X(WaitForFences) \
    X(ResetFences) X(QueueWaitIdle) X(DeviceWaitIdle) GE_COMMANDS(X)
#define DECLARE(name) PFN_vk##name name{};
struct Vulkan {
    Log& log;
    bool cleaned{};
    explicit Vulkan(Log& output) : log(output) {}
    INSTANCE_COMMANDS(DECLARE)
    DEVICE_COMMANDS(DECLARE)
    VkInstance instance{};
    VkDevice device{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swapchain{};
    VkRenderPass pass{};
    VkImageView views[8]{};
    VkFramebuffer buffers[8]{};
    VkCommandPool pool{};
    VkSemaphore acquired{}, rendered{};
    VkFence fence{};
    ~Vulkan() { if (!cleaned) cleanup(); }
    bool cleanup() {
        cleaned = true;
        VkResult idle = VK_SUCCESS;
        if (device && DeviceWaitIdle) {
            log.write("[vulkan] cleanup waiting for device idle");
            idle = DeviceWaitIdle(device);
            log.write("[vulkan] cleanup DeviceWaitIdle=%d", idle);
        }
        if (device) {
            if (fence && DestroyFence) DestroyFence(device, fence, nullptr);
            if (acquired && DestroySemaphore) DestroySemaphore(device, acquired, nullptr);
            if (rendered && DestroySemaphore) DestroySemaphore(device, rendered, nullptr);
            if (pool && DestroyCommandPool) DestroyCommandPool(device, pool, nullptr);
            for (auto framebuffer : buffers) if (framebuffer && DestroyFramebuffer) DestroyFramebuffer(device, framebuffer, nullptr);
            for (auto view : views) if (view && DestroyImageView) DestroyImageView(device, view, nullptr);
            if (pass && DestroyRenderPass) DestroyRenderPass(device, pass, nullptr);
            if (swapchain && DestroySwapchainKHR) DestroySwapchainKHR(device, swapchain, nullptr);
            if (DestroyDevice) DestroyDevice(device, nullptr);
        }
        if (surface && DestroySurfaceKHR) DestroySurfaceKHR(instance, surface, nullptr);
        if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
        log.write("[vulkan] cleanup complete; destroy calls returned (void API); idle=%d", idle);
        return idle == VK_SUCCESS;
    }
};
#undef DECLARE
bool has_extension(const VkExtensionProperties* items, unsigned count, const char* name) {
    for (unsigned i = 0; i < count; ++i) if (!std::strcmp(items[i].extensionName, name)) return true;
    return false;
}
}

static bool run_graphics(const TitlePaths& paths, Log& log
#if YAKUMO_WITH_INTEGRATION
    , const IntegrationGraphicsHooks* hooks=nullptr
#endif
    ) {
    Vulkan vk(log);
    GeGpu<Vulkan> ge_gpu(vk, log);
#define REQUIRE(test) do { if (!(test)) { log.write("[vulkan] FAIL %s line=%d", #test, __LINE__); return false; } } while (0)
#define RESULT(expr) do { const VkResult result = (expr); if (result != VK_SUCCESS) { log.write("[vulkan] FAIL %s result=%d line=%d", #expr, result, __LINE__); return false; } } while (0)
    char cache[path_capacity];
    REQUIRE(join_path(cache, sizeof(cache), paths.cache, "radv"));
    REQUIRE(setenv("MESA_SHADER_CACHE_DIR", cache, 1) == 0);
    char metadata[path_capacity];
    REQUIRE(join_path(metadata, sizeof(metadata), paths.root, "sce_sys/param.json"));
    REQUIRE(setenv("PS5_VIDEOOUT_PARAM_JSON", metadata, 1) == 0);
    log.write("[vulkan] cache=%s metadata=%s", cache, metadata);
    auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(vk_icdGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
    auto create = reinterpret_cast<PFN_vkCreateInstance>(vk_icdGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    REQUIRE(enumerate && create);
    VkExtensionProperties extensions[256]{};
    unsigned count = 256;
    RESULT(enumerate(nullptr, &count, extensions));
    REQUIRE(has_extension(extensions, count, VK_KHR_SURFACE_EXTENSION_NAME));
    REQUIRE(has_extension(extensions, count, VK_KHR_DISPLAY_EXTENSION_NAME));
    const char* required[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Yakumo-PS5 Probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount = 2;
    instance_info.ppEnabledExtensionNames = required;
    log.write("[vulkan] creating instance API=1.1");
    RESULT(create(&instance_info, nullptr, &vk.instance));
#define LOAD_I(name) vk.name = reinterpret_cast<PFN_vk##name>(vk_icdGetInstanceProcAddr(vk.instance, "vk" #name)); REQUIRE(vk.name);
    INSTANCE_COMMANDS(LOAD_I)
#undef LOAD_I
    VkPhysicalDevice physical[8]{};
    count = 8;
    RESULT(vk.EnumeratePhysicalDevices(vk.instance, &count, physical));
    REQUIRE(count > 0);
    const auto gpu = physical[0];
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceMemoryProperties memory{};
    vk.GetPhysicalDeviceProperties(gpu, &properties);
    vk.GetPhysicalDeviceFeatures(gpu, &features);
    vk.GetPhysicalDeviceMemoryProperties(gpu, &memory);
    log.write("[vulkan] device=%s API=%u.%u.%u maxImage2D=%u anisotropy=%u geometry=%u",
              properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
              VK_VERSION_PATCH(properties.apiVersion), properties.limits.maxImageDimension2D, features.samplerAnisotropy, features.geometryShader);
    REQUIRE(properties.apiVersion >= VK_API_VERSION_1_1);
    for (unsigned i = 0; i < memory.memoryHeapCount; ++i)
        log.write("[vulkan] heap=%u bytes=%llu flags=%#x", i, static_cast<unsigned long long>(memory.memoryHeaps[i].size), memory.memoryHeaps[i].flags);
    count = 256;
    RESULT(vk.EnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions));
    for (unsigned i = 0; i < count; ++i) log.write("[vulkan] extension=%s", extensions[i].extensionName);
    REQUIRE(has_extension(extensions, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
    for (const auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK}) {
        VkFormatProperties supported{};
        vk.GetPhysicalDeviceFormatProperties(gpu, format, &supported);
        log.write("[vulkan] format=%d optimal_features=%#x", format, supported.optimalTilingFeatures);
    }
    VkDisplayPropertiesKHR displays[8]{};
    count = 8;
    RESULT(vk.GetPhysicalDeviceDisplayPropertiesKHR(gpu, &count, displays));
    REQUIRE(count > 0);
    VkDisplayModePropertiesKHR modes[32]{};
    count = 32;
    RESULT(vk.GetDisplayModePropertiesKHR(gpu, displays[0].display, &count, modes));
    REQUIRE(count > 0);
    unsigned mode = 0;
    for (unsigned i = 0; i < count; ++i) {
        log.write("[vulkan] mode=%u width=%u height=%u refresh_mHz=%u", i, modes[i].parameters.visibleRegion.width, modes[i].parameters.visibleRegion.height, modes[i].parameters.refreshRate);
        if (modes[i].parameters.refreshRate >= 59000 && modes[i].parameters.refreshRate <= 61000) mode = i;
    }
    VkDisplayPlanePropertiesKHR planes[8]{};
    count = 8;
    RESULT(vk.GetPhysicalDeviceDisplayPlanePropertiesKHR(gpu, &count, planes));
    unsigned plane = count;
    for (unsigned i = 0; i < count; ++i) {
        VkDisplayKHR supported[8]{};
        unsigned n = 8;
        RESULT(vk.GetDisplayPlaneSupportedDisplaysKHR(gpu, i, &n, supported));
        for (unsigned j = 0; j < n; ++j) if (supported[j] == displays[0].display) plane = i;
    }
    REQUIRE(plane < count);
    VkDisplayPlaneCapabilitiesKHR plane_caps{};
    RESULT(vk.GetDisplayPlaneCapabilitiesKHR(gpu, modes[mode].displayMode, plane, &plane_caps));
    REQUIRE(plane_caps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR);
    VkDisplaySurfaceCreateInfoKHR surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
    surface_info.displayMode = modes[mode].displayMode;
    surface_info.planeIndex = plane;
    surface_info.planeStackIndex = planes[plane].currentStackIndex;
    surface_info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    surface_info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    surface_info.imageExtent = modes[mode].parameters.visibleRegion;
    log.write("[vulkan] creating display surface mode=%u plane=%u", mode, plane);
    RESULT(vk.CreateDisplayPlaneSurfaceKHR(vk.instance, &surface_info, nullptr, &vk.surface));
    VkQueueFamilyProperties families[32]{};
    count = 32;
    vk.GetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    unsigned family = count;
    for (unsigned i = 0; i < count; ++i) {
        VkBool32 present = VK_FALSE;
        RESULT(vk.GetPhysicalDeviceSurfaceSupportKHR(gpu, i, vk.surface, &present));
        if (present && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    }
    REQUIRE(family < count);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* swap_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = &swap_extension;
    log.write("[vulkan] creating device queue-family=%u", family);
    RESULT(vk.CreateDevice(gpu, &device_info, nullptr, &vk.device));
#define LOAD_D(name) vk.name = reinterpret_cast<PFN_vk##name>(vk.GetDeviceProcAddr(vk.device, "vk" #name)); REQUIRE(vk.name);
    DEVICE_COMMANDS(LOAD_D)
#undef LOAD_D
    VkQueue queue{};
    vk.GetDeviceQueue(vk.device, family, 0, &queue);
    VkSurfaceCapabilitiesKHR caps{};
    RESULT(vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, vk.surface, &caps));
    REQUIRE(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkSurfaceFormatKHR formats[32]{};
    count = 32;
    RESULT(vk.GetPhysicalDeviceSurfaceFormatsKHR(gpu, vk.surface, &count, formats));
    REQUIRE(count > 0);
    auto format = formats[0];
    if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
    VkSwapchainCreateInfoKHR swap{};
    swap.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swap.surface = vk.surface;
    swap.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && swap.minImageCount > caps.maxImageCount) swap.minImageCount = caps.maxImageCount;
    swap.imageFormat = format.format;
    swap.imageColorSpace = format.colorSpace;
    swap.imageExtent = caps.currentExtent.width == UINT32_MAX ? surface_info.imageExtent : caps.currentExtent;
    swap.imageArrayLayers = 1;
    swap.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swap.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swap.preTransform = caps.currentTransform;
    swap.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    REQUIRE(caps.supportedCompositeAlpha & swap.compositeAlpha);
    swap.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swap.clipped = VK_TRUE;
    log.write("[vulkan] creating swapchain images=%u", swap.minImageCount);
    RESULT(vk.CreateSwapchainKHR(vk.device, &swap, nullptr, &vk.swapchain));
    log.write("[vulkan] selected mode=%u refresh_mHz=%u extent=%ux%u format=%d colourspace=%d present=FIFO queue=%u",
              mode, modes[mode].parameters.refreshRate, swap.imageExtent.width, swap.imageExtent.height,
              swap.imageFormat, swap.imageColorSpace, family);
    VkImage images[8]{};
    count = 8;
    RESULT(vk.GetSwapchainImagesKHR(vk.device, vk.swapchain, &count, images));
    REQUIRE(count > 0 && count <= 8);
    REQUIRE(properties.limits.maxPushConstantsSize >= sizeof(ge::Push));
    REQUIRE(ge_gpu.setup(gpu, format.format));
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pool_info.queueFamilyIndex = family;
    RESULT(vk.CreateCommandPool(vk.device, &pool_info, nullptr, &vk.pool));
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = vk.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
    VkCommandBuffer command{};
    RESULT(vk.AllocateCommandBuffers(vk.device, &allocation, &command));
    VkSemaphoreCreateInfo semaphore{}; semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    RESULT(vk.CreateSemaphore(vk.device, &semaphore, nullptr, &vk.acquired));
    RESULT(vk.CreateSemaphore(vk.device, &semaphore, nullptr, &vk.rendered));
    VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    RESULT(vk.CreateFence(vk.device, &fence, nullptr, &vk.fence));
    timespec start{}, finish{};
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    log.write("[ge] begin 1200 frames: specialized 600, plain 600; six synthetic panels; waits=5s acquire/fence");
    unsigned submitted=0,checked=0;
    for (unsigned frame = 0; ; ++frame) {
#if YAKUMO_WITH_INTEGRATION
        if(hooks) {
            IntegrationSnapshot snapshot{};
            if(!hooks->frame(hooks->state,snapshot))break;
            if(snapshot.sequence) REQUIRE(ge_gpu.update_markers(integration_markers(snapshot)));
        } else
#endif
        if(frame>=ge::frames)break;
        unsigned index = 0;
        RESULT(vk.AcquireNextImageKHR(vk.device, vk.swapchain, 5000000000ULL, vk.acquired, VK_NULL_HANDLE, &index));
        REQUIRE(index < count);
        RESULT(vk.ResetCommandPool(vk.device, vk.pool, 0));
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        RESULT(vk.BeginCommandBuffer(command, &begin));
        ge_gpu.record(command, images[index], swap.imageExtent, frame);
        RESULT(vk.EndCommandBuffer(command));
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &vk.acquired; submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &vk.rendered;
        RESULT(vk.QueueSubmit(queue, 1, &submit, vk.fence));
        VkPresentInfoKHR present{}; present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &vk.rendered;
        present.swapchainCount = 1; present.pSwapchains = &vk.swapchain; present.pImageIndices = &index;
        RESULT(vk.QueuePresentKHR(queue, &present));
        RESULT(vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 5000000000ULL));
        // Serial diagnostic: ensures presentation consumed the binary semaphore before reuse.
        RESULT(vk.QueueWaitIdle(queue));
        RESULT(vk.ResetFences(vk.device, 1, &vk.fence));
        ++submitted;
        const unsigned phase=frame%ge::frames;
        if (phase == 0 || phase == 599 || phase == 600 || phase == 1199 || (ge_gpu.dynamic && frame%300==0)) {
            REQUIRE(ge_gpu.check(frame));
            ++checked;
#if YAKUMO_WITH_INTEGRATION
            if(hooks && ge_gpu.dynamic) hooks->rendered(hooks->state,true);
#endif
            log.write("[ge] presented=%u variant=%s pixels=PASS acquire/submit/present/fence/queue-idle=SUCCESS",
                      frame + 1, phase < 600 ? "specialized" : "plain");
        }
    }
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    const double seconds = static_cast<double>(finish.tv_sec - start.tv_sec) + (finish.tv_nsec - start.tv_nsec) / 1e9;
    REQUIRE(ge_gpu.cleanup());
    REQUIRE(vk.cleanup());
    REQUIRE(submitted>0 && checked>0);
#if YAKUMO_WITH_INTEGRATION
    if(hooks)log.write("[integration-ge] PASS submitted/presented=%u readbacks=%u cleanup seconds=%.3f; manual scanout unconfirmed",submitted,checked,seconds);
    else
#endif
    log.write("[ge] PASS 1200 submitted/presented frames, 60 pixel checks, cleanup seconds=%.3f; manual scanout unconfirmed",seconds);
    return true;
#undef REQUIRE
#undef RESULT
}
bool graphics_probe(const TitlePaths& paths,Log& log){return run_graphics(paths,log);}
#if YAKUMO_WITH_INTEGRATION
bool integration_graphics(const TitlePaths& paths,Log& log,IntegrationGraphicsHooks hooks){return run_graphics(paths,log,&hooks);}
#endif
}
