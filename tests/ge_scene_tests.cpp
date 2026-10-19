// SPDX-License-Identifier: GPL-3.0-or-later
#include "ge_scene.hpp"
#include <array>
#include <cstdio>
#include <cstring>

int main() {
    using namespace yakumo::ps5::ge;
    Scene scene;
    if (scene.vertex_count != 66 || scene.draw_count != 11) return 1;
    for (unsigned i = 0; i < scene.draw_count; ++i) {
        const auto& d = scene.draws[i];
        if (d.first + 6 > scene.vertex_count) return 2;
        const auto p = push(d);
        if (p.texture[0] != (d.texture ? 1 : 0) || p.texture[3] != (d.alpha ? 6 : 0)) return 3;
        if (p.viewport[2] != (d.through ? 1 : 0)) return 4;
    }
    // Independent numeric checks: transformed z .25 -> -.5 -> Vulkan .25;
    // through cyan is pixel-space, with depth remapped by the real shader.
    if (scene.vertices[24].z != -.5f || scene.vertices[30].z != .5f) return 5;
    if (scene.vertices[60].x != 344 || scene.vertices[60].y != 160 || scene.vertices[60].z != 32767.5f) return 6;
    std::array<std::uint8_t, width * height * 4> pixels{};
    for (const auto& s : samples) {
        auto* p = pixels.data() + (s.y * width + s.x) * 4;
        p[0] = s.r; p[1] = s.g; p[2] = s.b;
        if (!matches(pixels.data(), s)) return 7;
        const auto saved = p[0];
        p[0] = saved < 128 ? saved + 4 : saved - 4;
        if (matches(pixels.data(), s)) return 8;
        p[0] = saved < 128 ? saved + 3 : saved - 3;
        if (!matches(pixels.data(), s)) return 9;
        p[0] = saved;
    }
    std::puts("GE scene layout, coordinate/depth contracts and pixel oracle boundaries PASS");
}
