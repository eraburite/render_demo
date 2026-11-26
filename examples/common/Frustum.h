#pragma once
#include <array>
#include <cmath>
#include <glm/glm.hpp>

// View-frustum planes and the AABB test that goes with them.
//
// The reference implementations (LearnOpenGL guest 2021 1.scene) build the six
// planes from the camera's basis vectors: front, up and right.  That silently
// assumes a symmetric projection and no roll, and it re-derives what the
// projection matrix already encodes -- so any mismatch between the two shows up
// as geometry that the CPU thinks is visible but the GPU clips (or the reverse).
//
// Extracting the planes from the view-projection matrix instead makes the test
// agree with the same matrix the vertex shader multiplies by.  Row combinations
// of a column-major clip matrix give the six half-spaces directly:
//
//     left   = m[3] + m[0]      right  = m[3] - m[0]
//     bottom = m[3] + m[1]      top    = m[3] - m[1]
//     near   = m[3] + m[2]      far    = m[3] - m[2]
//
// where m[i] is column i of the matrix and the result is (nx, ny, nz, d) with
// the inside half-space being nx*x + ny*y + nz*z + d >= 0.
namespace frustum {

struct Plane {
    // (nx, ny, nz, d) as above, normalised so `d` is a metric distance.
    glm::vec4 coefficients{0.0f, 0.0f, 0.0f, 0.0f};

    float signedDistance(const glm::vec3 &point) const
    {
        return coefficients.x * point.x + coefficients.y * point.y + coefficients.z * point.z
               + coefficients.w;
    }
};

struct Frustum {
    std::array<Plane, 6> planes{};
};

// Conservative with respect to Vulkan's [0, 1] clip depth: the planes are
// derived for the [-1, 1] clip volume the projection matrix actually produces
// (see RenderCommon.h: glm::perspective + a Y flip, no depth remap).  A near
// plane at z_ndc = -1 lies *behind* the one the GPU clips at (z_ndc = 0), so
// anything this test accepts is accepted by the GPU too.  The reverse -- culling
// something the GPU would have drawn -- cannot happen, which is what makes the
// cull/no-cull dump comparison in the example a valid invariant.
inline Frustum fromViewProjection(const glm::mat4 &viewProjection)
{
    // Column-major: viewProjection[column][row].
    const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
    const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
    const glm::vec4 row2(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
    const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);

    Frustum result;
    const std::array<glm::vec4, 6> raw{{
        row3 + row0, // left
        row3 - row0, // right
        row3 + row1, // bottom
        row3 - row1, // top
        row3 + row2, // near
        row3 - row2, // far
    }};
    for (size_t i = 0; i < raw.size(); ++i) {
        const glm::vec3 normal(raw[i]);
        const float length = glm::length(normal);
        // A degenerate row means a malformed matrix; leaving the plane at zero
        // makes signedDistance() return 0, i.e. "on the surface", which accepts
        // everything rather than silently culling the whole scene.
        result.planes[i].coefficients = length > 0.0f ? raw[i] / length : glm::vec4(0.0f);
    }
    return result;
}

// Box-versus-plane test using the projection radius of the box onto the plane
// normal: if the box centre is further than -radius behind the plane it is
// outside.  Testing all six planes accepts a superset of the true intersection
// (the classic "false positive" corner case), which is the safe direction.
inline bool intersects(const Frustum &frustum, const glm::vec3 &center, const glm::vec3 &extent)
{
    for (const Plane &plane : frustum.planes) {
        const float radius = extent.x * std::abs(plane.coefficients.x)
                             + extent.y * std::abs(plane.coefficients.y)
                             + extent.z * std::abs(plane.coefficients.z);
        if (plane.signedDistance(center) < -radius) {
            return false;
        }
    }
    return true;
}

// World-space AABB of a local AABB under an affine transform.  The extent of the
// transformed box along each world axis is the absolute-value matrix times the
// local extent, which is exact for axis-aligned scaling+rotation and conservative
// for shear (there is none here: node transforms are TRS).
struct WorldBox {
    glm::vec3 center{0.0f};
    glm::vec3 extent{0.0f};
};

inline WorldBox transformBox(const glm::mat4 &transform, const glm::vec3 &localCenter,
                             const glm::vec3 &localExtent)
{
    WorldBox box;
    box.center = glm::vec3(transform * glm::vec4(localCenter, 1.0f));
    const glm::mat3 linear(transform);
    // Component-wise abs, i.e. |R| * extent: this is exact for a rotation with a
    // per-axis scale and is never smaller than the true extent.
    const glm::mat3 absolute(glm::abs(linear[0]), glm::abs(linear[1]), glm::abs(linear[2]));
    box.extent = absolute * localExtent;
    return box;
}

} // namespace frustum
