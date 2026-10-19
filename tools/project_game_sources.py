#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project the locked MIT sources into a separate GPL native build workspace."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
UP = ROOT / 'upstream/Yakumo'
PIN = '03e7ca2d5ebd1fe4e7958235f4041716e3637cb6'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Projection anchor is not unique: {old[:100]!r}')
    return text.replace(old, new)


def body_end(text, opening):
    # Braces in C++ literals/comments must not terminate a projected function.
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|(?<![\w])\'(?:\\.|[^\'\\\n])*\'|[{}]')
    depth = 0
    for m in token.finditer(text, opening):
        if m[0] == '{':
            depth += 1
        elif m[0] == '}':
            depth -= 1
            if depth == 0:
                return m.end()
    raise ValueError('Unterminated function')


def function(text, signature, body):
    if text.count(signature) != 1:
        raise ValueError(f'Function anchor is not unique: {signature}')
    opening = text.index('{', text.index(signature))
    end = body_end(text, opening)
    return text[:opening] + '{\n' + body + '\n}' + text[end:]


def renderer(text, performance=False):
    text = text.replace('#include <SDL3/SDL.h>', '#include "game_platform.hpp"\n#include "text_input_service.hpp"\n#include "game_text_observer.hpp"\n#include "game_text_background.hpp"\n#include "psprecomp/common.hpp"')
    text = text.replace('#include <SDL3/SDL_vulkan.h>', '')
    text = text.replace('#include "backends/imgui_impl_vulkan.h"', '')
    text = text.replace('#include "imgui.h"', '')
    text = replace(text, '#include "input/gamepad_devices.hpp"', '')
    text = replace(text, '        std::_Exit(3);', '        throw psprecomp::Error("Vulkan device lost; native lifecycle owns closure");')
    a = text.index('// The bindings name a pad\'s buttons by SDL\'s numbers.')
    b = text.index('// Background texture decoding', a)
    text = text[:a] + '// Controller sampling is provided by the native backend.\n' + text[b:]
    a = text.index('    // MHP3RD_PAD_FOLLOW=0:')
    b = text.index('    [[nodiscard]] std::uint32_t find_memory_type', a)
    text = text[:a] + text[b:]
    text = text.replace('SDL_JoystickID', 'std::uint32_t')
    for signature in ('    void update_touch_layout()', '    void handle_touch(const SDL_Event &event)',
                      '    void update_pointer(bool focused)', 'void VulkanRenderer::Impl::sample_free_camera(bool focused)',
                      'void VulkanRenderer::Impl::sample_host_binds(bool focused)',
                      'void VulkanRenderer::Impl::resolve_bindings(bool focused)',
                      'void VulkanRenderer::Impl::sample_hide_hud(bool focused)',
                      'void VulkanRenderer::Impl::sample_lock_on(bool focused)'):
        text = function(text, signature, '// No desktop mouse, touchscreen or host shortcuts in this target.')
    text = function(text, 'bool VulkanRenderer::pump_events()', 'return impl_ && !impl_->quit;')
    text = function(text, 'void VulkanRenderer::Impl::sample_pad(bool focused)',
                    '''pad = game_input ? yakumo::game_platform::sample_pad() : PadState{};
    if (!pad.native_input_ready) lead_in.reset();
    pad.buttons = lead_in.apply(pad.buttons, pad.native_lead_requested,
        game_read || std::getenv("MHP3RD_PAD_AT_FLIP") != nullptr);
    if (pad.hide_hud) hide_hud_pressed = true;
    static const bool disabled = std::getenv("YAKUMO_OSK_LIVE_BACKGROUND") != nullptr;
    if (text_capture.sample(pad.buttons, yakumo::text_input::service().blocks_input()) && !disabled)
        cache_text_background();''')
    text = replace(text, '    bool holding{};', '    bool holding{};\n    bool text_hold_attempted{};\n    std::uint64_t text_capture_frame{};\n    yakumo::game_text_background::CaptureEdge text_capture;\n    yakumo::game_text_background::Recovery text_recovery;\n    void cache_text_background();')
    signature = 'void VulkanRenderer::hold_frame(bool hold)'
    start = text.index(signature)
    end = body_end(text, text.index('{', start))
    text = text[:start] + (ROOT / 'src/game_text_frame.inc').read_text() + text[end:]
    text = replace(text, '    if (impl.holding) source = impl.held.color;',
                   '    if (std::getenv("YAKUMO_OSK_LIVE_BACKGROUND") == nullptr)\n'
                   '        hold_frame(yakumo::game_text_observer::background_requested);\n'
                   '    if (impl.holding) source = impl.held.color;')
    text = replace(text, 'std::uint32_t &height) {\n    if (!impl_) return false;',
                   'std::uint32_t &height, bool live_probe) {\n    if (!impl_) return false;')
    text = replace(text, '    const VkImage captured = shown->second.color;',
                   '    const VkImage captured = !live_probe && impl.holding ? impl.held.color : shown->second.color;')
    text = replace(text, '    height = impl.target_extent.height;\n    const VkDeviceSize bytes',
                   '    height = impl.target_extent.height;\n    if (live_probe) width = height = 8;\n    const VkDeviceSize bytes')
    text = replace(text, '    vkCmdCopyImageToBuffer(commands, captured, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1u, &copy);',
                   '''    if (live_probe) {
        std::array<VkBufferImageCopy,64> samples{};
        for (std::uint32_t i=0; i<samples.size(); ++i) {
            const auto point=yakumo::game_text_background::sample_point(i,impl.target_extent.width,impl.target_extent.height);
            samples[i].bufferOffset=i*4;
            samples[i].imageSubresource=copy.imageSubresource;
            samples[i].imageOffset={static_cast<int>(point.x),static_cast<int>(point.y),0};
            samples[i].imageExtent={1,1,1};
        }
        vkCmdCopyImageToBuffer(commands,captured,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging,samples.size(),samples.data());
    } else vkCmdCopyImageToBuffer(commands, captured, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1u, &copy);''')
    text = replace(text, '    vkAllocateMemory(impl.device, &allocate, nullptr, &staging_memory);',
                   '    if (vkAllocateMemory(impl.device, &allocate, nullptr, &staging_memory) != VK_SUCCESS) { vkDestroyBuffer(impl.device, staging, nullptr); return false; }')
    text = replace(text, '    vkBindBufferMemory(impl.device, staging, staging_memory, 0u);',
                   '    if (vkBindBufferMemory(impl.device, staging, staging_memory, 0u) != VK_SUCCESS) { vkDestroyBuffer(impl.device, staging, nullptr); vkFreeMemory(impl.device, staging_memory, nullptr); return false; }')
    text = replace(text, '    vkMapMemory(impl.device, staging_memory, 0u, bytes, 0u, &mapped);',
                   '    if (vkMapMemory(impl.device, staging_memory, 0u, bytes, 0u, &mapped) != VK_SUCCESS) { vkDestroyBuffer(impl.device, staging, nullptr); vkFreeMemory(impl.device, staging_memory, nullptr); return false; }')
    text = replace(text, '    if (holding) {\n        holding = false;\n        destroy_target(held);',
                   '    if (held.color) {\n        holding = false;\n        text_hold_attempted = false;\n        text_recovery.cancel();\n        destroy_target(held);')
    text = function(text, 'void VulkanRenderer::sample_pad()',
                    'if (impl_) impl_->sample_pad(true);')
    text = function(text, 'PadState VulkanRenderer::pad() const noexcept',
                    'return yakumo::text_input::service().blocks_input() ? PadState{} : impl_ ? impl_->pad : PadState{};')
    text = text.replace('SDL_GetTicks()', 'yakumo::game_platform::milliseconds()')
    text = text.replace('vkWaitForFences(device, 1u, &fence, VK_TRUE, UINT64_MAX)',
                        "vkWaitForFences(device, 1u, &fence, VK_TRUE, 5'000'000'000ull)")
    text = function(text, '    void wait_fence(VkFence fence, const char *where)',
                    '''const VkResult rc=vkWaitForFences(device,1,&fence,VK_TRUE,5'000'000'000ull);
if(rc!=VK_SUCCESS) throw psprecomp::Error(std::string("Fence wait ")+where+" rc="+std::to_string(rc));''')
    a = text.index('    if (!SDL_Init(SDL_INIT_VIDEO))')
    b = text.index('    // Only a loader that offers portability', a)
    text = text[:a] + '    std::vector<const char *> extensions = yakumo::game_platform::instance_extensions();\n' + text[b:]
    a = text.index('    if (!SDL_Vulkan_CreateSurface(impl.window, impl.instance, nullptr, &impl.surface))')
    end = body_end(text, text.index('{', a))
    text = text[:a] + '    if (!yakumo::game_platform::create_surface(impl.instance, impl.surface, error)) return false;\n' + text[end:]
    text = text.replace('vkGetPhysicalDeviceSurfaceSupportKHR(impl.physical_device, i, impl.surface, &present);',
                        'if (impl.surface) vkGetPhysicalDeviceSurfaceSupportKHR(impl.physical_device, i, impl.surface, &present); else present = VK_TRUE;')
    text = text.replace('log_device(impl.physical_device, impl.surface, impl.facts);',
                        'if (impl.surface) log_device(impl.physical_device, impl.surface, impl.facts);')
    text = text.replace('    vkGetDeviceQueue(impl.device, impl.queue_family, 0u, &impl.queue);',
                        '    yakumo::game_dispatch::load_device(impl.device);\n    vkGetDeviceQueue(impl.device, impl.queue_family, 0u, &impl.queue);')
    text = replace(text, '    if (!yakumo::game_platform::create_surface(impl.instance, impl.surface, error)) return false;',
                   '    yakumo::game_dispatch::load_instance(impl.instance);\n    if (!yakumo::game_platform::create_surface(impl.instance, impl.surface, error)) return false;')
    a = text.index('    // Swapchain.\n')
    b = text.index('    if (!impl.create_swapchain(error))', a)
    text = text[:a] + '    if (impl.surface) {\n' + text[a:b] + '    }\n' + text[b:]
    # Offscreen host is a genuine GE renderer without a presentation engine.
    sig = 'bool VulkanRenderer::Impl::create_swapchain(std::string &error)'
    at = text.index('{', text.index(sig)) + 1
    text = text[:at] + '\n    if (!surface) { swapchain_extent = {480,272}; update_content_rect(); swapchain_dirty=false; return true; }\n' + text[at:]
    text = function(text, 'void VulkanRenderer::Impl::update_display_info()',
                    'display_hz = yakumo::game_platform::display_hz();' +
                    ('\nperf::set_display_info(present_mode_name(present_mode), swapchain_extent.width, swapchain_extent.height, display_hz);' if performance else ''))
    text = function(text, 'void VulkanRenderer::set_window_scale(std::uint32_t scale)', '(void)scale;')
    text = function(text, 'void VulkanRenderer::set_fullscreen(bool fullscreen)', '(void)fullscreen;')
    text = function(text, 'bool VulkanRenderer::initialize_ui(std::string &error)',
                    'error = "Host ImGui menu is unavailable in the first-frame target"; return false;')
    text = function(text, 'void VulkanRenderer::shutdown_ui()', 'if (impl_) impl_->ui_ready = false;')
    text = function(text, 'void VulkanRenderer::begin_ui_frame()', '')
    text = function(text, 'void VulkanRenderer::set_ui_draw_data(ImDrawData *draw_data)', '(void)draw_data;')
    text = function(text, 'void VulkanRenderer::present_ui(bool show_game)',
                    '(void)show_game; throw psprecomp::Error("Host UI presentation is unavailable");')
    text = function(text, 'void VulkanRenderer::Impl::submit_and_present(\n    VkCommandBuffer commands',
                    (ROOT / ('src/game_performance_present.inc' if performance else 'src/game_present.inc')).read_text())
    if performance:
        text = function(text, 'double VulkanRenderer::Impl::wanted_rate() const', '''return yakumo::game_performance::capped_rate(pacing::requested_rate(frame_rate, display_hz), display_hz);''')
        text = replace(text, '    std::optional<std::uint32_t> acquired_image;',
                       '    bool native_present_succeeded{};\n    std::optional<std::uint32_t> acquired_image;')
        text = replace(text, '    submit_and_present(commands, fence, image, true, false);',
                       '    submit_and_present(commands, fence, image, true, false);\n'
                       '    if (!native_present_succeeded) { ++stats.blocked; return false; }\n'
                       '    if (blended) ++yakumo::game_performance::blended;')
        text = replace(text, '    stats = InterpolationStats{};\n    stats.window_start = now;',
                       '    yakumo::game_performance::skipped += stats.skipped + stats.over_budget;\n'
                       '    yakumo::game_performance::blocked += stats.blocked;\n'
                       '    stats = InterpolationStats{};\n    stats.window_start = now;')
        text = replace(text, '    if (!impl.recording) begin_frame();\n    impl.end_pass();\n\n    static const bool trace3d',
                       '    if (!impl.recording) begin_frame();\n'
                       '    yakumo::game_performance::resident = impl.replacements.resident_bytes();\n'
                       '    yakumo::game_performance::resident_count = impl.replacements.resident_count();\n'
                       '    impl.end_pass();\n\n    static const bool trace3d')
        text = function(text, 'std::filesystem::path VulkanRenderer::textures_root()',
                        'return yakumo::game_files::root() / "yakumo/textures";')
    text = text.replace('for (const auto &[id, device] : impl_->pads) SDL_CloseGamepad(device);', '')
    text = text.replace('SDL_DestroyWindow(impl_->window);', '')
    text = text.replace('if (impl.ui_ready && ImGui::GetCurrentContext() != nullptr) ImGui_ImplVulkan_Shutdown();', '')
    text = text.replace('if (impl.window != nullptr) SDL_DestroyWindow(impl.window);', '')
    text = text.replace('SDL_Log(', 'std::printf(')
    text = replace(text, '        ImGui_ImplVulkan_SetMinImageCount(swapchain_min_images);', '        (void)swapchain_min_images;')
    text = text.replace('SDL_GetWindowSizeInPixels(window, &width, &height);',
                        'yakumo::game_platform::display_size(width, height);')
    text = text.replace('install::user_data_directory() / "pipeline_cache.bin"',
                        'yakumo::game_files::root() / "yakumo/cache/pipeline_cache.bin"')
    text = text.replace('install::user_data_directory() / "pipeline_keys.bin"',
                        'yakumo::game_files::root() / "yakumo/cache/pipeline_keys.bin"')
    if performance:
        text = text.replace('yakumo::game_files::root() / "yakumo/cache/pipeline_cache.bin"',
                            'yakumo::game_performance::cache_directory() / "pipeline_cache.bin"')
        text = text.replace('yakumo::game_files::root() / "yakumo/cache/pipeline_keys.bin"',
                            'yakumo::game_performance::cache_directory() / "pipeline_keys.bin"')
    return text


def performance_source(name, text):
    if name.endswith('/settings/settings.cpp'):
        text = '#include "game_pad.hpp"\n#include "game_files.hpp"\n' + text
        text = replace(text, '    Settings values{};', '''    Settings values{};
    values.internal_scale = 6;
    values.texture_pack = true;
    values.crisp_text = true;
    values.adhoc_server = "socom.cc";
    values.frame_rate = FrameRate::Fps60;
    values.frame_rate_auto = true;
    values.perf = PerfDisplay::Off;
    values.fast_forward = fast_forward::Mode::Hold;
    values.fast_forward_speed = 4;
    values.touch_controls = false;
    values.font = (yakumo::game_files::root() / "yakumo/assets/fonts/NotoSansCJKjp-Regular.otf").string();
    values.controls = yakumo::game_pad::native_layout(values.controls);''')
        text = replace(text, 'constexpr int kVersion = 2;', 'constexpr int kVersion = 3;')
        text = replace(text, '    return entries.erase("text.crisp") != 0u;', '    return false;')
        text = text.replace('Sharp text (text.crisp) off by default.',
                            'Crisp text on by default; explicit saved values are retained.')
        text = text.replace('text.crisp from an earlier version dropped: Sharp text is off by default now; "\n'
                            '                     "turn it on in Video if you want it',
                            'settings version upgraded; explicit saved text.crisp value retained')
        text = replace(text, '    settle_controls(values, entries);',
                       '    settle_controls(values, entries);\n    values.controls = yakumo::game_pad::native_layout(values.controls);')
        text = replace(text, '    values.controls = yakumo::game_pad::native_layout(values.controls);\n}\n\nvoid load(State &s)',
                       '''    values.controls = yakumo::game_pad::native_layout(values.controls);
    std::uint32_t server_version = 1u;
    if (const auto version = entries.find(kVersionKey); version != entries.end())
        (void)parse_uint(version->second, 1u, 1000u, server_version);
    // Older releases persisted the empty server default in every settings file.
    // Upgrade only those legacy empty values; version 3 can intentionally stay empty.
    if (values.adhoc_server.empty() && server_version < 3u) values.adhoc_server = "socom.cc";
}\n\nvoid load(State &s)''')
        text = replace(text, '        if (input::layout(*s.control_preset.shipped) == s.controls) return;',
                       '        if (yakumo::game_pad::native_layout(input::layout(*s.control_preset.shipped)) ==\n'
                       '            yakumo::game_pad::native_layout(s.controls)) return;')
    if name.endswith('/perf/frame_stats.cpp'):
        text = replace(text, '    if (options().log) print(out);',
                       '    if (options().log) print(out);\n    yakumo::game_performance::summary(out);')
    if name.endswith('/gpu/texture_pack.cpp'):
        text = replace(text, '    if (!folded_scanned_) {', '''    // Prepared packs provide their names without a recursive metadata walk.
    // Keep the original fallback for an unprepared custom pack.
    if (!folded_scanned_) {
        std::ifstream index(directory_ / "runtime-case-index.txt", std::ios::binary | std::ios::ate);
        if (index && index.tellg() >= 0 && index.tellg() <= 4*1024*1024) {
            index.seekg(0, std::ios::beg);
            std::string relative;
            std::size_t count = 0;
            while (count++ < 8192 && std::getline(index, relative)) {
                if (relative.empty() || relative.size() > 900 || relative.find('\\\\') != std::string::npos) continue;
                const auto path = path_from_utf8(relative);
                bool safe = !path.is_absolute();
                for (const auto &part : path) safe &= part != ".." && part != ".";
                if (safe) folded_names_.emplace(lower(relative), directory_ / path);
            }
            folded_scanned_ = true;
        }
    }
    if (!folded_scanned_) {''')
        text = replace(text, 'void TexturePack::decode(Replacement &replacement) {',
                       'void TexturePack::decode(Replacement &replacement) {\n'
                       '    const auto decode_start = yakumo::game_performance::now_us();')
        text = replace(text, '        std::cerr << "[texpack] cannot use "',
                       '        if (yakumo::game_performance::allow_failure_log()) std::cerr << "[texpack] cannot use "')
        text = replace(text, '    replacement.pixels = std::move(pixels);',
                       '    yakumo::game_performance::decoded_bytes += pixels.size();\n'
                       '    ++yakumo::game_performance::decoded;\n'
                       '    yakumo::game_performance::decode_us += yakumo::game_performance::now_us() - decode_start;\n'
                       '    replacement.pixels = std::move(pixels);')
    if name.endswith('/gpu/replacement_textures.cpp'):
        text = replace(text, '    resident.upload_fence = upload.fence;',
                       '    ++yakumo::game_performance::uploaded;\n    resident.upload_fence = upload.fence;')
        text = replace(text, '        resident_bytes_ -= resident->bytes;',
                       '        ++yakumo::game_performance::evicted;\n        resident_bytes_ -= resident->bytes;')
        text = replace(text, '        vkWaitForFences(device_.device, 1u, &upload.fence, VK_TRUE, UINT64_MAX);',
                       "        yakumo::game_performance::require_upload_finished(vkWaitForFences(device_.device, 1u, &upload.fence, VK_TRUE, 5'000'000'000ull));")
    needs_header = 'yakumo::game_performance::' in text
    if 'std::printf(' in text:
        text = text.replace('std::printf(', 'yakumo::game_performance::log_printf(')
        needs_header = True
    return ('#include "game_performance.hpp"\n' if needs_header else '') + text


def project(output, performance=False):
    from prepare_textures import reject_links
    reject_links(output.absolute())
    if not output.resolve().is_relative_to((ROOT / "out/data").resolve()):
        raise ValueError("Projected sources must stay under ignored out/data")
    head = subprocess.check_output(['git', '-C', str(UP), 'rev-parse', 'HEAD'], text=True).strip()
    status = subprocess.check_output(['git', '-C', str(UP), 'status', '--porcelain'], text=True)
    if head != PIN or status.strip():
        raise ValueError('Projection requires the clean locked upstream checkout')
    expected = json.loads((ROOT / 'tools/game_projection_inputs.json').read_text())
    sources = {}
    commands = set()
    for name, digest in expected.items():
        data = (UP / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('Projection input changed: ' + name)
        text = data.decode('utf-8')
        commands.update(re.findall(r'\b(vk[A-Z][A-Za-z0-9_]*)\s*\(', text))
        if name.endswith(('.cpp', '.hpp')):
            if name.endswith('/gpu/vulkan_renderer.hpp'):
                text = replace(text, '    bool fast_forward{};',
                               '    bool fast_forward{};\n    bool hide_hud{}; // Native fresh-press pulse; never a guest button.\n    std::uint32_t native_lead_requested{};\n    bool native_input_ready{};')
                text = replace(text, '    bool read_frame(std::vector<std::uint8_t> &pixels, std::uint32_t &width, std::uint32_t &height);',
                               '    bool read_frame(std::vector<std::uint8_t> &pixels, std::uint32_t &width, std::uint32_t &height, bool live_probe = false);\n    void recover_text_background();')
            if name.endswith('/gpu/vulkan_renderer.cpp'):
                text = renderer(text, performance)
            if performance:
                text = performance_source(name, text)
            if name.endswith('/gpu/device_report.cpp'):
                text = text.replace('#include <SDL3/SDL.h>', '#include <cstdio>')
                text = text.replace('SDL_Log(', 'std::printf(')
            if name.endswith('/fonts/game_font.cpp'):
                text = re.sub(r'const char \*const kFontCandidates\[\] = \{[\s\S]*?\n\};',
                              'const std::array<const char*, 0> kFontCandidates{};', text, count=1)
                text = re.sub(r'const char \*const kFontFolders\[\] = \{[\s\S]*?\n\};',
                              'const std::array<const char*, 0> kFontFolders{};', text, count=1)
                text = function(text, '    bool open(const std::filesystem::path &path)', '''close();
std::ifstream input(path, std::ios::binary | std::ios::ate);
if(!input) return false;
const auto length=input.tellg();
if(length<=0 || length>64*1024*1024) return false;
owned_.resize(static_cast<std::size_t>(length));
input.seekg(0,std::ios::beg);
input.read(reinterpret_cast<char*>(owned_.data()),static_cast<std::streamsize>(owned_.size()));
if(!input || input.gcount()!=static_cast<std::streamsize>(owned_.size())) { owned_.clear(); return false; }
size_=owned_.size(); data_=owned_.data(); return true;''')
                text = function(text, '    void close()', 'data_=nullptr; size_=0; owned_.clear();')
                text = replace(text, '    const std::uint8_t *data_{};',
                               '    std::vector<std::uint8_t> owned_;\n    const std::uint8_t *data_{};')
            if name.endswith('/app_paths.cpp'):
                text = function(text, 'std::filesystem::path executable_path()',
                                'return yakumo::game_files::root() / "eboot.bin";')
                text = function(text, 'std::filesystem::path bundled_font_directory()',
                                'return yakumo::game_files::root() / "yakumo/assets/fonts";')
            if name == 'src/runtime.cpp':
                text = replace(text, '    std::string relative = psp_path;',
                               '''    for (const auto& part: std::filesystem::path(psp_path))
        if (part == "..") throw Error("Native guest path traversal rejected");
    std::string relative = psp_path;''')
            if name.endswith('/hle/hle_io.cpp'):
                text = replace(text, "    std::replace(rest.begin(), rest.end(), '\\\\', '/');", '''    if(rest.find('\\\\')!=std::string::npos){result.device=Device::Unknown;return result;}
    for(const auto& component:std::filesystem::path(rest))
        if(component==".."){result.device=Device::Unknown;return result;}''')
                text = replace(text, '        file.size = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0u;',
                               '''        file.host->seekg(0, std::ios::end);
        const auto end_position = file.host->tellg();
        if(end_position < 0) return static_cast<std::int32_t>(io_error::kBadFileDescriptor);
        file.size = static_cast<std::uint64_t>(end_position);
        file.host->seekg(0, std::ios::beg);''')
            if name.endswith('/kernel/iso_image.cpp'):
                text = replace(text, '    size_bytes_ = std::filesystem::file_size(path);',
                               '''    file_.seekg(0, std::ios::end);
    const auto end_position = file_.tellg();
    if(end_position < 0) throw psprecomp::Error("Cannot size validated ISO descriptor");
    size_bytes_ = static_cast<std::uint64_t>(end_position);
    file_.seekg(0, std::ios::beg);''')
            if name.endswith('/install/user_data.cpp'):
                text = function(text, 'std::filesystem::path user_data_directory()',
                                'return yakumo::game_files::root() / "yakumo/config";')
            if name.endswith('/audio/audio_sink.cpp'):
                text = '#include "game_platform.hpp"\n' + text
                text = replace(text, 'std::cout << "Audio: built without SDL; running silent\\n";',
                               '''if (!yakumo::game_platform::audio_start([&impl](std::int16_t* output, unsigned frames) {
    std::lock_guard<std::mutex> guard(impl.lock); impl.retire(frames, output, true);
})) std::cout << "Audio: native playback unavailable\\n";''')
                text = replace(text, '    impl.closed = true;', '    impl.closed = true;\n    yakumo::game_platform::audio_stop();')
                text = replace(text, '    impl.gain = effective;', '    impl.gain = effective;\n    yakumo::game_platform::audio_gain(effective);')
                text = replace(text, '    return false;\n#endif\n}', '    return yakumo::game_platform::audio_running();\n#endif\n}')
                text = replace(text, '    (void)paused;', '    yakumo::game_platform::audio_pause(paused);')
                text = replace(text, '    const bool draining = false;', '    const bool draining = yakumo::game_platform::audio_running();')
            if name.endswith('/hle/hle_media.cpp'):
                anchor = '    const bool presented = renderer.present(address, kernel().real_time_of(frame_start_us(kernel().last_vblank_us())));'
                text = replace(text, anchor, anchor + '\n    renderer.recover_text_background();')
                text = replace(text, '#include "ui/ui.hpp"', '#include "game_ui.hpp"')
                text = replace(text, 'std::cerr << "Renderer: unavailable (" << error << "); running headless\\n";\n        return nullptr;',
                               'throw psprecomp::Error("Native renderer initialization failed: " + error);')
            if name.endswith('/hle/hle_mpeg.cpp'):
                text = replace(text, '        register_movie_skip(hle);',
                               '        std::cerr << "[game-codecs] MPEG unavailable; movie-skip HLE handlers are not registered\\n";')
            if name.endswith('/kernel/fast_forward.cpp'):
                text = '#include "game_pad.hpp"\n#include "text_input_service.hpp"\n' + text
                text = replace(text, '    return guards;',
                               '    return yakumo::game_pad::fast_forward_guards(guards, yakumo::text_input::service().blocks_input());')
            if name.endswith('/hle/hle_utility.cpp'):
                text = '#include "text_input_service.hpp"\n#include "game_text_observer.hpp"\n#include "game_text_policy.hpp"\n#include "game_platform.hpp"\n' + text
                a = text.index('// The on-screen keyboard\'s parameters')
                b = text.index('// SceUtilityMsgDialogParams', a)
                text = text[:a] + '#include "game_osk.inc"\n\n' + text[b:]
            if name.endswith(('/hle/hle_utility.cpp', '/hle/hle_savedata.cpp')):
                text = text.replace('defined(MHP3RD_HAS_RENDERER)', 'defined(MHP3RD_HAS_HOST_UI)')
            text = text.replace('#include "ui/ui.hpp"', '#include "game_ui.hpp"')
            text = text.replace('std::filesystem::', 'yakumo::game_files::')
            text = text.replace('namespace fs = std::filesystem;', 'namespace fs = yakumo::game_files;')
            for stream in ('ifstream', 'ofstream', 'fstream'):
                text = text.replace('std::' + stream, 'yakumo::game_files::' + stream)
            text = text.replace('std::fopen(', 'yakumo::game_files::fopen(')
            text = '#include "game_files.hpp"\n#include "game_vk_dispatch.hpp"\n' + text
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        encoded = text.encode('utf-8')
        if not dest.exists() or dest.read_bytes() != encoded:
            dest.write_bytes(encoded)
        sources[name] = dict(input_sha256=digest, output_sha256=hashlib.sha256(encoded).hexdigest())
    manifest = dict(upstream_commit=PIN, license='MIT upstream; GPL-3.0-or-later projection/adapters', sources=sources)
    manifest['performance'] = performance
    commands.update(re.findall(r'\b(vk[A-Z][A-Za-z0-9_]*)\s*\(', (ROOT/'src/game_platform.cpp').read_text()))
    commands.discard('vk_icdGetInstanceProcAddr')
    declarations = '\n'.join(f'inline PFN_{name} {name}{{}};' for name in sorted(commands))
    load = '\n'.join(f'    if (auto p=get("{name}")) ::{name}=reinterpret_cast<PFN_{name}>(p);' for name in sorted(commands))
    header = '''// Generated ICD dispatch for the native projection; no Vulkan loader import.
#pragma once
#include <stdexcept>
#if YAKUMO_NATIVE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance,const char*);
''' + declarations + '\nnamespace yakumo::game_dispatch {\n' + '''
template<class Getter> inline void load(Getter get) {
''' + load + '''
}
inline void load_global(){load([](const char* n){return vk_icdGetInstanceProcAddr(nullptr,n);});if(!vkCreateInstance||!vkEnumerateInstanceExtensionProperties)throw std::runtime_error("Required RADV global entry point unavailable");}
inline void load_instance(VkInstance i){load([&](const char* n){return vk_icdGetInstanceProcAddr(i,n);});if(!vkEnumeratePhysicalDevices||!vkCreateDevice||!vkGetDeviceProcAddr)throw std::runtime_error("Required RADV instance entry point unavailable");}
inline void load_device(VkDevice d){load([&](const char* n){return vkGetDeviceProcAddr(d,n);});if(!vkQueueSubmit||!vkCreateGraphicsPipelines||!vkCreateSwapchainKHR)throw std::runtime_error("Required RADV device entry point unavailable");}
}
#else
#include <vulkan/vulkan.h>
namespace yakumo::game_dispatch {
inline void load_global(){}
inline void load_instance(VkInstance){}
inline void load_device(VkDevice){}
}
#endif
'''
    target = output/'game_vk_dispatch.hpp'
    if not target.exists() or target.read_text()!=header:
        target.write_text(header)
    (output / 'projection.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Projected', len(sources), 'locked source/header inputs; upstream remains clean')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--performance', action='store_true')
    args = parser.parse_args()
    project(args.output, args.performance)
