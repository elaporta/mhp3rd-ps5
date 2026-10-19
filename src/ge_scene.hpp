// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace yakumo::ps5::ge {
constexpr unsigned width = 480, height = 272, frames = 1200;
// Interface facts from pinned Yakumo ge.vert/ge.frag and GpuVertex.
struct Vertex { float x, y, z, w, u, v; std::uint32_t color; float nx, ny, nz; };
struct Push { float transform[16], viewport[4], texture[4], uv[4], view_z[4]; };
static_assert(sizeof(Vertex) == 40 && offsetof(Vertex, color) == 24 && offsetof(Vertex, nx) == 28);
static_assert(sizeof(Push) == 128 && offsetof(Push, viewport) == 64 && offsetof(Push, view_z) == 112);
struct Draw { unsigned first; bool texture, alpha, blend, through; };
struct Scene {
    Vertex vertices[96]{};
    Draw draws[16]{};
    unsigned vertex_count{}, draw_count{};
    void quad(float x, float y, float w, float h, float z, std::uint32_t color,
              bool texture = false, bool alpha = false, bool blend = false, bool through = false) {
        draws[draw_count++] = {vertex_count, texture, alpha, blend, through};
        constexpr unsigned corners[] = {0, 1, 2, 2, 1, 3};
        for (unsigned c : corners) {
            const float px = x + (c & 1 ? w : 0), py = y + (c & 2 ? h : 0);
            vertices[vertex_count++] = {through ? px : px / width * 2 - 1,
                through ? py : py / height * 2 - 1, through ? z * 65535 : z * 2 - 1,
                through ? 1e20f : 1.0f, c & 1 ? 1.0f : 0.0f, c & 2 ? 1.0f : 0.0f,
                color, -1e20f, -1e20f, 1e20f};
        }
    }
    Scene() {
        quad(8, 8, 48, 112, .5f, 0xff0000ff); // RGB vertex colours
        quad(56, 8, 48, 112, .5f, 0xff00ff00);
        quad(104, 8, 48, 112, .5f, 0xffff0000);
        quad(168, 8, 144, 112, .5f, 0xffffffff, true);
        quad(352, 32, 80, 64, .25f, 0xff00ff00); // near first, far second
        quad(328, 8, 144, 112, .75f, 0xff0000ff);
        quad(8, 144, 144, 112, .75f, 0xffff0000);
        quad(32, 168, 96, 64, .25f, 0x800000ff, false, false, true);
        quad(168, 144, 144, 112, .75f, 0xff00ffff); // yellow behind discarded white texel
        quad(168, 144, 144, 112, .25f, 0xffffffff, true, true);
        quad(344, 160, 112, 80, .5f, 0xffffff00, false, false, false, true);
    }
};
constexpr std::uint32_t texels[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0x00ffffff};
struct Sample { unsigned x, y; int r, g, b; const char* name; };
struct Markers {
    unsigned x[2]{};
    std::uint32_t color[2]{};
    Sample expected[2]{};
};
// Independent interior pixel expectations, away from edges and interpolation.
constexpr Sample samples[] = {
    {24, 64, 255, 0, 0, "vertex-red"}, {80, 64, 0, 255, 0, "vertex-green"},
    {128, 64, 0, 0, 255, "vertex-blue"}, {200, 32, 255, 0, 0, "texture-TL"},
    {280, 32, 0, 255, 0, "texture-TR"}, {200, 96, 0, 0, 255, "texture-BL"},
    {280, 96, 255, 255, 255, "texture-BR-no-alpha-test"},
    {392, 64, 0, 255, 0, "depth-near-survives-later-far"}, {336, 16, 255, 0, 0, "depth-far-border"},
    {80, 200, 128, 0, 127, "source-alpha-blend"}, {16, 152, 0, 0, 255, "blend-background"},
    {200, 168, 255, 0, 0, "alpha-opaque"}, {280, 224, 255, 255, 0, "alpha-discard"},
    {400, 200, 0, 255, 255, "through-cyan"}, {336, 152, 0, 0, 0, "through-border"}
};
inline bool matches(const std::uint8_t* rgba, const Sample& sample) {
    const auto* p = rgba + (sample.y * width + sample.x) * 4;
    return std::abs(int(p[0]) - sample.r) <= 3 && std::abs(int(p[1]) - sample.g) <= 3 &&
           std::abs(int(p[2]) - sample.b) <= 3;
}
inline Push push(const Draw& draw) {
    Push p{};
    p.transform[0] = p.transform[5] = p.transform[10] = p.transform[15] = 1;
    p.viewport[0] = width; p.viewport[1] = height; p.viewport[2] = draw.through ? 1 : 0;
    p.texture[0] = draw.texture ? 1 : 0; p.texture[1] = 3;
    p.texture[2] = 128; p.texture[3] = draw.alpha ? 6 : 0;
    p.uv[0] = p.uv[1] = 1;
    return p;
}
}
