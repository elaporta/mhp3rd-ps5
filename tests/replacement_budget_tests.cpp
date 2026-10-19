// SPDX-License-Identifier: GPL-3.0-or-later
// Real Vulkan uploads/LRU on synthetic decoded images; no game data.
#include "gpu/replacement_textures.hpp"
#include "perf/frame_stats.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace mhp3rd::perf { bool alternate_off(NewPath) { return false; } }
void check(bool ok) { if(!ok)throw std::runtime_error("replacement budget assertion failed"); }
void vkcheck(VkResult rc) { check(rc==VK_SUCCESS); }
int main() {
    using namespace mhp3rd::gpu;
    setenv("MHP3RD_TEXTURE_PACK_MEMORY","64",1);
    VkInstance instance{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;
    vkcheck(vkCreateInstance(&ii,nullptr,&instance));
    std::uint32_t count{};vkcheck(vkEnumeratePhysicalDevices(instance,&count,nullptr));check(count>0);
    std::vector<VkPhysicalDevice> physical(count);vkcheck(vkEnumeratePhysicalDevices(instance,&count,physical.data()));
    const auto gpu=physical.front();
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());
    std::uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;check(family<count);
    const float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VkDevice device{};vkcheck(vkCreateDevice(gpu,&di,nullptr,&device));
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkDescriptorSetLayoutBinding binding{};binding.descriptorCount=1;binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&binding;
    VkDescriptorSetLayout layout{};vkcheck(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
    const auto root=std::filesystem::temp_directory_path()/("yakumo-budget-"+std::to_string(getpid()));
    std::filesystem::create_directories(root);
    std::ofstream(root/"textures.ini")<<"[options]\nhash=xxh64\n[hashes]\n000000000000000100000000=synthetic.png\n";
    std::string error;auto pack=TexturePack::open(root,"NPJB40001",error);check(bool(pack));
    ReplacementTextures textures;check(textures.initialize({gpu,device,queue,family,layout},error));
    std::array<Replacement,4> images;
    for(auto& image:images) {
        image.width=image.height=2048;
        image.pixels.assign(2048u*2048u*4u,255);
        image.state.store(Replacement::State::Decoded);
    }
    check(textures.descriptor(images[0],*pack,1)!=VK_NULL_HANDLE);
    check(textures.descriptor(images[1],*pack,1)!=VK_NULL_HANDLE);
    check(textures.descriptor(images[2],*pack,1)==VK_NULL_HANDLE); // upload budget defers; original can draw
    check(images[2].state==Replacement::State::Decoded);
    check(textures.descriptor(images[2],*pack,2)!=VK_NULL_HANDLE);
    check(textures.descriptor(images[3],*pack,2)!=VK_NULL_HANDLE);
    vkcheck(vkDeviceWaitIdle(device));
    check(textures.resident_count()==4&&textures.resident_bytes()>64ull*1024*1024);
    textures.begin_frame(3);check(textures.resident_count()==4); // keep recent/in-flight users
    textures.begin_frame(10);check(textures.resident_count()<4&&textures.resident_bytes()<=56ull*1024*1024);
    bool retired=false;for(auto& image:images)retired|=image.state==Replacement::State::Unloaded&&image.gpu==nullptr;check(retired);
    Replacement failed;failed.state.store(Replacement::State::Failed);
    check(textures.descriptor(failed,*pack,11)==VK_NULL_HANDLE);
    textures.clear();check(!textures.resident_count()&&!textures.resident_bytes());
    textures.shutdown();pack.reset();
    vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    std::filesystem::remove_all(root);
    std::cout<<"real Vulkan replacement upload budget, retention, LRU and failure fallback PASS\n";
}
