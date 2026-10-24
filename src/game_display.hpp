// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <vector>

namespace yakumo::game_display {
// Resolution fallback never increases the requested size. The pinned VideoOut
// driver opens high refresh once per process and advertises it only after a
// vblank check. Select that effective refresh even for a 60-FPS presentation
// request; choosing the nominal 60-Hz alias would misreport a 120-Hz output.
inline std::vector<std::size_t> candidates(const std::vector<VkDisplayModePropertiesKHR>& modes,
                                          unsigned requested_height) {
    std::vector<std::size_t> result;
    for (const auto height : {2160u, 1440u, 1080u}) {
        if (height > requested_height) continue;
        const unsigned width = height == 2160 ? 3840 : height == 1440 ? 2560 : 1920;
        std::vector<std::size_t> matching;
        for (std::size_t i=0;i<modes.size();++i) {
            const auto& p=modes[i].parameters;
            if (p.visibleRegion.width==width && p.visibleRegion.height==height &&
                p.refreshRate>=1000 && p.refreshRate<=120500) matching.push_back(i);
        }
        std::stable_sort(matching.begin(),matching.end(),[&](auto a,auto b) {
            return modes[a].parameters.refreshRate>modes[b].parameters.refreshRate;
        });
        result.insert(result.end(),matching.begin(),matching.end());
    }
    return result;
}
}
