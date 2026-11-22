#pragma once
#include <cstdint>
#include <vector>
#include "VertexInputStateConfig.h"

// 36-vertex cube with per-face normals / UVs (no index buffer).
//
// UV convention: for every face, (u, v) = (0, 0) is the TOP-LEFT corner when
// that face is viewed from outside the cube.  Vulkan images and stb_image both
// store row 0 at the top, so sampling with these UVs displays textures upright
// without flipping pixels in loadTexture() or the V coordinate in any shader.
//
// The previous UVs were authored with the OpenGL convention (V = 0 at the
// geometric bottom) and were not even consistent across faces: the +/-Z and
// +/-Y faces were upside down, and the +/-X faces had U/V swapped (90-degree
// rotation).  Face order, positions, winding and normals are unchanged.
inline std::vector<Vertex> CubeVertices(const glm::vec3 &color = glm::vec3(1.0f))
{
    using V = Vertex;
    const glm::vec3 c = color;
    return {
        // -Z face: viewed from -Z, +Y is up and -X is screen-right.
        V{{-0.5f, -0.5f, -0.5f}, c, {1, 1}, {0, 0, -1}}, {{0.5f, -0.5f, -0.5f}, c, {0, 1}, {0, 0, -1}},
        V{{0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 0, -1}},  {{0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 0, -1}},
        V{{-0.5f, 0.5f, -0.5f}, c, {1, 0}, {0, 0, -1}}, {{-0.5f, -0.5f, -0.5f}, c, {1, 1}, {0, 0, -1}},

        // +Z face: viewed from +Z, +Y is up and +X is screen-right.
        V{{-0.5f, -0.5f, 0.5f}, c, {0, 1}, {0, 0, 1}},  {{0.5f, -0.5f, 0.5f}, c, {1, 1}, {0, 0, 1}},
        V{{0.5f, 0.5f, 0.5f}, c, {1, 0}, {0, 0, 1}},    {{0.5f, 0.5f, 0.5f}, c, {1, 0}, {0, 0, 1}},
        V{{-0.5f, 0.5f, 0.5f}, c, {0, 0}, {0, 0, 1}},   {{-0.5f, -0.5f, 0.5f}, c, {0, 1}, {0, 0, 1}},

        // -X face: viewed from -X, +Y is up and +Z is screen-right.
        V{{-0.5f, 0.5f, 0.5f}, c, {1, 0}, {-1, 0, 0}},  {{-0.5f, 0.5f, -0.5f}, c, {0, 0}, {-1, 0, 0}},
        V{{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {-1, 0, 0}}, {{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {-1, 0, 0}},
        V{{-0.5f, -0.5f, 0.5f}, c, {1, 1}, {-1, 0, 0}}, {{-0.5f, 0.5f, 0.5f}, c, {1, 0}, {-1, 0, 0}},

        // +X face: viewed from +X, +Y is up and -Z is screen-right.
        V{{0.5f, 0.5f, 0.5f}, c, {0, 0}, {1, 0, 0}},   {{0.5f, 0.5f, -0.5f}, c, {1, 0}, {1, 0, 0}},
        V{{0.5f, -0.5f, -0.5f}, c, {1, 1}, {1, 0, 0}}, {{0.5f, -0.5f, -0.5f}, c, {1, 1}, {1, 0, 0}},
        V{{0.5f, -0.5f, 0.5f}, c, {0, 1}, {1, 0, 0}},  {{0.5f, 0.5f, 0.5f}, c, {0, 0}, {1, 0, 0}},

        // -Y face: viewed from -Y, +Z is up and +X is screen-right.
        // This face already used the correct convention; kept as-is.
        V{{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {0, -1, 0}}, {{0.5f, -0.5f, -0.5f}, c, {1, 1}, {0, -1, 0}},
        V{{0.5f, -0.5f, 0.5f}, c, {1, 0}, {0, -1, 0}},   {{0.5f, -0.5f, 0.5f}, c, {1, 0}, {0, -1, 0}},
        V{{-0.5f, -0.5f, 0.5f}, c, {0, 0}, {0, -1, 0}},  {{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {0, -1, 0}},

        // +Y face: viewed from +Y, -Z is up and +X is screen-right.
        V{{-0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 1, 0}}, {{0.5f, 0.5f, -0.5f}, c, {1, 0}, {0, 1, 0}},
        V{{0.5f, 0.5f, 0.5f}, c, {1, 1}, {0, 1, 0}},   {{0.5f, 0.5f, 0.5f}, c, {1, 1}, {0, 1, 0}},
        V{{-0.5f, 0.5f, 0.5f}, c, {0, 1}, {0, 1, 0}},  {{-0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 1, 0}},
    };
}

inline std::vector<uint32_t> CubeIndices()
{
    return {0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4, 8, 9, 10, 10, 11, 8,
            12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};
}
