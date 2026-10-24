// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual native selection boundary with unordered/rejected modes.
#include "game_platform.hpp"
#include "game_files.hpp"
#include "game_settings.hpp"
#include "game_performance.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance,const char*) { return nullptr; }
namespace {
std::vector<VkDisplayModePropertiesKHR> offered;
bool refuse_4k{};
unsigned attempts{};
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void offer(unsigned width,unsigned height,unsigned refresh) {
    VkDisplayModePropertiesKHR mode{};
    mode.displayMode=(VkDisplayModeKHR)(std::uintptr_t)(offered.size()+1);
    mode.parameters={{width,height},refresh};offered.push_back(mode);
}
}
int main(int argc,char** argv) { try {
    namespace fs=std::filesystem;
    namespace platform=yakumo::game_platform;
    namespace menu=yakumo::game_settings;
    const std::string test=argc>1?argv[1]:"default";
    const auto root=fs::temp_directory_path()/("yakumo-display-"+std::to_string(getpid()));
    fs::create_directories(root/"yakumo/config");yakumo::game_files::set_root(root);
    const auto output=test=="default"?"1080p":test=="1440"?"1440p":"2160p";
    if(test!="default") std::ofstream(root/"yakumo/config/settings.ini")<<"ps5.output_resolution="<<output<<'\n';
    mhp3rd::settings::current().frame_rate=test=="default"?mhp3rd::settings::FrameRate::Fps60:mhp3rd::settings::FrameRate::Fps120;
    const bool baseline=test=="baseline-only";
    const bool low=test=="refresh-fallback" || baseline, small=test=="resolution-fallback" || baseline;
    offer(1920,1080,59940);
    if(!small && !low) offer(3840,2160,119880);
    if(!baseline) offer(2560,1440,59940);
    if(!low) offer(1920,1080,119880);
    if(!small) offer(3840,2160,59940);
    if(!low) offer(2560,1440,119880);
    if(test=="none") {offered.clear();offer(480,272,59940);}
    refuse_4k=test=="refused";
    vkEnumeratePhysicalDevices=+[](VkInstance,std::uint32_t* n,VkPhysicalDevice* p) -> VkResult {
        if(p) *p=(VkPhysicalDevice)1;*n=1;return VK_SUCCESS;
    };
    vkGetPhysicalDeviceProperties=+[](VkPhysicalDevice,VkPhysicalDeviceProperties* p) {p->deviceType=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;};
    vkGetPhysicalDeviceDisplayPropertiesKHR=+[](VkPhysicalDevice,std::uint32_t* n,VkDisplayPropertiesKHR* p) -> VkResult {
        if(p) p->display=(VkDisplayKHR)1;*n=1;return VK_SUCCESS;
    };
    vkGetDisplayModePropertiesKHR=+[](VkPhysicalDevice,VkDisplayKHR,std::uint32_t* n,VkDisplayModePropertiesKHR* p) -> VkResult {
        if(p) std::copy(offered.begin(),offered.end(),p);*n=offered.size();return VK_SUCCESS;
    };
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR=+[](VkPhysicalDevice,std::uint32_t* n,VkDisplayPlanePropertiesKHR* p) -> VkResult {
        if(p) *p={};*n=1;return VK_SUCCESS;
    };
    vkGetDisplayPlaneSupportedDisplaysKHR=+[](VkPhysicalDevice,std::uint32_t,std::uint32_t* n,VkDisplayKHR* p) -> VkResult {
        if(p) *p=(VkDisplayKHR)1;*n=1;return VK_SUCCESS;
    };
    vkGetDisplayPlaneCapabilitiesKHR=+[](VkPhysicalDevice,VkDisplayModeKHR,std::uint32_t,VkDisplayPlaneCapabilitiesKHR* p) -> VkResult {
        p->supportedAlpha=VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;return VK_SUCCESS;
    };
    vkCreateDisplayPlaneSurfaceKHR=+[](VkInstance,const VkDisplaySurfaceCreateInfoKHR* p,const VkAllocationCallbacks*,VkSurfaceKHR* s) -> VkResult {
        ++attempts;
        if(refuse_4k && p->imageExtent.height==2160) return VK_ERROR_INITIALIZATION_FAILED;
        *s=(VkSurfaceKHR)1;return VK_SUCCESS;
    };
    VkSurfaceKHR surface=VK_NULL_HANDLE;std::string error;
    const bool success=platform::create_surface(VK_NULL_HANDLE,surface,error);
    if(test=="none") check(!success && !error.empty() && surface==VK_NULL_HANDLE && !attempts,"missing modes fail without a bogus surface");
    else {
        check(success && surface!=VK_NULL_HANDLE,"native surface selection");
        int width{},height{};platform::display_size(width,height);
        const int expected=test=="default" || baseline?1080:(test=="1440" || small || refuse_4k)?1440:2160;
        check(height==expected && width==(expected==2160?3840:expected==1440?2560:1920),"effective selected dimensions");
        check(std::abs(platform::display_hz()-(low?59.94:119.88))<0.01,"effective driver refresh, independent of enumeration order");
        check(std::abs(yakumo::game_performance::capped_rate(test=="default"?60:120,platform::display_hz())-
                       (test=="default"?60:low?59.94:119.88))<0.01,"refresh cap and unchanged 60-FPS default");
        if(refuse_4k) check(attempts==3,"rejected 4K surfaces fall back through reported modes");
        // Saved settings remain requested, even when the driver needs a fallback.
        check(menu::output_resolution()==output,"fallback preserves the saved request");
    }
    fs::remove_all(root);
    std::cout<<"native display "<<test<<" PASS\n";return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
