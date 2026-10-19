// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "game_vk_dispatch.hpp"
#include "gpu/vulkan_renderer.hpp"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace yakumo::game_platform {
std::vector<const char*> instance_extensions();
bool create_surface(VkInstance, VkSurfaceKHR&, std::string&);
void display_size(int&, int&);
float display_hz();
std::uint64_t milliseconds();
mhp3rd::gpu::PadState sample_pad();
void count_present();
std::uint64_t present_count();
bool initialize_devices();
void shutdown_devices();
bool audio_start(std::function<void(std::int16_t*, unsigned)> consume);
void audio_stop();
bool audio_running();
int audio_error();
void audio_pause(bool);
void audio_gain(float);
}
