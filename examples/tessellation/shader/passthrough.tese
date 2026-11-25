#version 450

layout (set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
} ubo;

// 0 = left half of the screen, 1 = right half.
layout (constant_id = 0) const uint uSide = 0u;

layout (triangles, fractional_odd_spacing, cw) in;

layout (location = 0) in vec3 inNormal[];
layout (location = 0) out vec3 outNormal;

#define uvw gl_TessCoord

void main()
{
    // Barycentric interpolation of the three patch control points.  The
    // (z, x, y) -> (P0, P1, P2) permutation matches the PN evaluation shader
    // for the "cw" tessellation domain, so both halves show the same base
    // triangle and only differ by the PN displacement.
    vec3 pos = uvw.z * gl_in[0].gl_Position.xyz
             + uvw.x * gl_in[1].gl_Position.xyz
             + uvw.y * gl_in[2].gl_Position.xyz;

    vec4 clip = ubo.viewProj * vec4(pos, 1.0);
    // NDC x in [-1,1] -> [-1,0] (left) or [0,1] (right).
    float side = float(uSide) * 2.0 - 1.0;
    clip.x = 0.5 * clip.x + 0.5 * side * clip.w;
    gl_Position = clip;

    outNormal = normalize(uvw.z * inNormal[0] + uvw.x * inNormal[1] + uvw.y * inNormal[2]);
}
