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
// Winding: every face is counter-clockwise when viewed from outside, which is
// what vk::FrontFace::eCounterClockwise + eBack culling expects.  The -Z, +X and
// +Y faces used to emit their two triangles in the opposite order: their
// cross(B - A, C - A) pointed *against* the stored normal, so any pipeline with
// back-face culling dropped exactly those three faces.  Nothing noticed while
// every example ran with cullMode = eNone; the texturecubemap example (skybox
// culls eFront, objects cull eBack) exposed it immediately.
inline std::vector<Vertex> CubeVertices(const glm::vec3 &color = glm::vec3(1.0f))
{
    using V = Vertex;
    const glm::vec3 c = color;
    return {
        // -Z face: viewed from -Z, +Y is up and -X is screen-right.
        V{{-0.5f, -0.5f, -0.5f}, c, {1, 1}, {0, 0, -1}}, {{0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 0, -1}},
        V{{0.5f, -0.5f, -0.5f}, c, {0, 1}, {0, 0, -1}},  {{0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 0, -1}},
        V{{-0.5f, -0.5f, -0.5f}, c, {1, 1}, {0, 0, -1}}, {{-0.5f, 0.5f, -0.5f}, c, {1, 0}, {0, 0, -1}},

        // +Z face: viewed from +Z, +Y is up and +X is screen-right.
        V{{-0.5f, -0.5f, 0.5f}, c, {0, 1}, {0, 0, 1}},  {{0.5f, -0.5f, 0.5f}, c, {1, 1}, {0, 0, 1}},
        V{{0.5f, 0.5f, 0.5f}, c, {1, 0}, {0, 0, 1}},    {{0.5f, 0.5f, 0.5f}, c, {1, 0}, {0, 0, 1}},
        V{{-0.5f, 0.5f, 0.5f}, c, {0, 0}, {0, 0, 1}},   {{-0.5f, -0.5f, 0.5f}, c, {0, 1}, {0, 0, 1}},

        // -X face: viewed from -X, +Y is up and +Z is screen-right.
        V{{-0.5f, 0.5f, 0.5f}, c, {1, 0}, {-1, 0, 0}},  {{-0.5f, 0.5f, -0.5f}, c, {0, 0}, {-1, 0, 0}},
        V{{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {-1, 0, 0}}, {{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {-1, 0, 0}},
        V{{-0.5f, -0.5f, 0.5f}, c, {1, 1}, {-1, 0, 0}}, {{-0.5f, 0.5f, 0.5f}, c, {1, 0}, {-1, 0, 0}},

        // +X face: viewed from +X, +Y is up and -Z is screen-right.
        V{{0.5f, 0.5f, 0.5f}, c, {0, 0}, {1, 0, 0}},   {{0.5f, -0.5f, -0.5f}, c, {1, 1}, {1, 0, 0}},
        V{{0.5f, 0.5f, -0.5f}, c, {1, 0}, {1, 0, 0}},  {{0.5f, -0.5f, -0.5f}, c, {1, 1}, {1, 0, 0}},
        V{{0.5f, 0.5f, 0.5f}, c, {0, 0}, {1, 0, 0}},   {{0.5f, -0.5f, 0.5f}, c, {0, 1}, {1, 0, 0}},

        // -Y face: viewed from -Y, +Z is up and +X is screen-right.
        V{{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {0, -1, 0}}, {{0.5f, -0.5f, -0.5f}, c, {1, 1}, {0, -1, 0}},
        V{{0.5f, -0.5f, 0.5f}, c, {1, 0}, {0, -1, 0}},   {{0.5f, -0.5f, 0.5f}, c, {1, 0}, {0, -1, 0}},
        V{{-0.5f, -0.5f, 0.5f}, c, {0, 0}, {0, -1, 0}},  {{-0.5f, -0.5f, -0.5f}, c, {0, 1}, {0, -1, 0}},

        // +Y face: viewed from +Y, -Z is up and +X is screen-right.
        V{{-0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 1, 0}}, {{0.5f, 0.5f, 0.5f}, c, {1, 1}, {0, 1, 0}},
        V{{0.5f, 0.5f, -0.5f}, c, {1, 0}, {0, 1, 0}},  {{0.5f, 0.5f, 0.5f}, c, {1, 1}, {0, 1, 0}},
        V{{-0.5f, 0.5f, -0.5f}, c, {0, 0}, {0, 1, 0}}, {{-0.5f, 0.5f, 0.5f}, c, {0, 1}, {0, 1, 0}},
    };
}

// CubeVertices() is already an expanded 36-vertex triangle list (each face's
// 6th vertex repeats its 3rd).  The matching index buffer is therefore the
// identity permutation; an old 24-vertex cube table used to live here and
// produced degenerate triangles / missing faces if ever paired with these
// vertices.  Every example currently draws non-indexed, so this is provided
// for completeness only.
inline std::vector<uint32_t> CubeIndices()
{
    std::vector<uint32_t> indices(36);
    for (uint32_t i = 0; i < 36; ++i) {
        indices[i] = i;
    }
    return indices;
}
