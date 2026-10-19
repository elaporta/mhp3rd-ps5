// SPDX-License-Identifier: GPL-3.0-or-later
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "probe.hpp"
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <dlfcn.h>
#include "ge_gpu.hpp"
#include "integration_markers.hpp"

#define HOST_COMMANDS(X) \
    X(DestroyDevice) X(DeviceWaitIdle) X(CreateImageView) X(DestroyImageView) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences) X(ResetFences) GE_COMMANDS(X)
#define FIELD(name) PFN_vk##name name{};
struct API {
    VkDevice device{};
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties{};
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties{};
    HOST_COMMANDS(FIELD)
};
#undef FIELD
static VkResult VKAPI_CALL fail_pipeline(VkDevice, VkPipelineCache, std::uint32_t,
    const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
int main() {
    using namespace yakumo::ps5;
    Log log;
    void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "Vulkan host loader unavailable: %s\n", dlerror()); return 1; }
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    if (!get) return 2;
#define GET_I(name) const auto name = reinterpret_cast<PFN_vk##name>(get(instance, "vk" #name)); if (!name) return 3;
    VkInstance instance{};
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &app;
    if (!create || create(&ci, nullptr, &instance)) return 4;
    GET_I(DestroyInstance) GET_I(EnumeratePhysicalDevices) GET_I(GetPhysicalDeviceProperties)
    GET_I(GetPhysicalDeviceQueueFamilyProperties) GET_I(CreateDevice) GET_I(GetDeviceProcAddr)
    VkPhysicalDevice gpu{}; unsigned count = 1;
    if (EnumeratePhysicalDevices(instance, &count, &gpu) != VK_SUCCESS || !count) return 5;
    VkPhysicalDeviceProperties properties{}; GetPhysicalDeviceProperties(gpu, &properties);
    log.write("[host-ge] device=%s (host evidence only)", properties.deviceName);
    VkQueueFamilyProperties families[32]{}; count = 32; GetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    unsigned family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == count) return 6;
    const float priority = 1;
    VkDeviceQueueCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{}; di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    API api;
    if (CreateDevice(gpu, &di, nullptr, &api.device)) return 7;
#define LOAD_D(name) api.name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(api.device, "vk" #name)); if (!api.name) return 8;
    HOST_COMMANDS(LOAD_D)
#undef LOAD_D
    api.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(instance,"vkGetPhysicalDeviceMemoryProperties"));
    api.GetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(get(instance,"vkGetPhysicalDeviceFormatProperties"));
    const auto queue_get = reinterpret_cast<PFN_vkGetDeviceQueue>(GetDeviceProcAddr(api.device,"vkGetDeviceQueue"));
    VkQueue queue{}; queue_get(api.device, family, 0, &queue);
    VkCommandPool pool{}; VkCommandPoolCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = family;
    if (api.CreateCommandPool(api.device, &pi, nullptr, &pool)) return 9;
    VkCommandBuffer cmd{}; VkCommandBufferAllocateInfo ca{}; ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    if (api.AllocateCommandBuffers(api.device, &ca, &cmd)) return 10;
    VkFence fence{}; VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (api.CreateFence(api.device, &fi, nullptr, &fence)) return 11;
    bool success = true;
    {
        GeGpu<API> ge(api, log);
        success = ge.setup(gpu, VK_FORMAT_R8G8B8A8_UNORM);
        for (unsigned frame : {0u,599u,600u,1199u,1200u,1799u,2400u,3599u}) {
            if (!success) break;
            if(frame>=1200) {
                IntegrationSnapshot state{};
                state.sequence=frame;state.module=frame%32;state.x=int(frame%255)-127;state.connected=frame!=2400;
                state.guest=((state.sequence+unsigned(state.x+127)+state.module+3)^(state.module*17+5))+1013;
                if(!ge.update_markers(integration_markers(state))) {success=false;break;}
            }
            VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (api.ResetCommandPool(api.device, pool, 0) || api.BeginCommandBuffer(cmd, &begin)) { success = false; break; }
            ge.record(cmd, VK_NULL_HANDLE, {}, frame);
            if (api.EndCommandBuffer(cmd)) { success = false; break; }
            VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
            if (api.QueueSubmit(queue, 1, &submit, fence) || api.WaitForFences(api.device, 1, &fence, VK_TRUE, 5000000000ULL) ||
                api.ResetFences(api.device, 1, &fence) || !ge.check(frame)) success = false;
        }
        if (success) {
            void* mapped{};
            if (api.MapMemory(api.device, ge.readback.memory, 0, ge::width * ge::height * 4, 0, &mapped)) success = false;
            else {
                auto* rgba = static_cast<std::uint8_t*>(mapped);
                if (const char* path = std::getenv("YAKUMO_GE_REFERENCE")) {
                    FILE* capture = std::fopen(path, "wb");
                    if (!capture) success = false;
                    else {
                        if (std::fprintf(capture, "P6\n%u %u\n255\n", ge::width, ge::height) < 0) success = false;
                        for (unsigned i = 0; i < ge::width * ge::height; ++i)
                            if (std::fwrite(rgba + i * 4, 1, 3, capture) != 3) success = false;
                        if (std::fclose(capture)) success = false;
                    }
                }
                rgba[(64 * ge::width + 24) * 4] = 0; // Deliberately corrupt the red sample.
                api.UnmapMemory(api.device, ge.readback.memory);
                if (ge.check(1199)) success = false;
                log.write("[host-ge] intentionally wrong pixel rejected");
                // A wrong dynamic marker must also be refused independently.
                if(api.MapMemory(api.device,ge.readback.memory,0,ge::width*ge::height*4,0,&mapped))success=false;
                else {static_cast<std::uint8_t*>(mapped)[(64*ge::width+24)*4]=255;api.UnmapMemory(api.device,ge.readback.memory);}
                auto wrong=ge.markers;wrong.expected[0].r=123;
                ge.markers=wrong;
                if(ge.check(3599))success=false;
                log.write("[host-ge] intentionally wrong dynamic marker rejected; frames beyond 1200 exercised");
            }
        }
        success &= ge.cleanup();
    }
    if (success) {
        const auto original = api.CreateGraphicsPipelines;
        api.CreateGraphicsPipelines = fail_pipeline;
        { GeGpu<API> partial(api, log);
          if (partial.setup(gpu, VK_FORMAT_R8G8B8A8_UNORM)) success = false;
          success &= partial.cleanup(); }
        api.CreateGraphicsPipelines = original;
        log.write("[host-ge] injected pipeline failure rejected and partial resources cleaned");
    }
    api.DestroyFence(api.device, fence, nullptr);
    api.DestroyCommandPool(api.device, pool, nullptr);
    api.DestroyDevice(api.device, nullptr); DestroyInstance(instance, nullptr); dlclose(library);
    log.write("[host-ge] %s; software Vulkan only, no PS5 evidence", success ? "PASS" : "FAIL");
    return success ? 0 : 12;
}
