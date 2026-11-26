#version 450

// CPU-displaced terrain: every vertex already carries its final world position
// and its shading normal, computed on the host from the heightmap.  No
// tessellation stage is involved, so the mesh resolution is exactly the one the
// host built.
//
// The varyings are deliberately identical to terrain_gpu.tese so both paths run
// the same fragment shader -- the only thing that differs between the two modes
// is *who* produced the geometry.

layout (set = 1, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    mat4 view;
    vec4 eye;     // xyz: camera position in world space
    vec4 height;  // x: height span, y: height shift, z: grayscale flag
    vec4 terrain; // x: world width, y: world depth, z: one texel in u, w: one texel in v
    vec4 tess;    // x: min level, y: max level, z: min distance, w: max distance
} ubo;

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outWorldPos;
layout (location = 2) out float outHeight;

void main()
{
    outWorldPos = inPos;
    outNormal = inNormal;
    outHeight = inPos.y;
    gl_Position = ubo.viewProj * vec4(inPos, 1.0);
}
