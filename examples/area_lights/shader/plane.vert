#version 450

// Ground plane of the LTC area-light example (7.area_lights).  The plane is one
// 20x20 quad centred at the origin in world space, so no model matrix is needed:
// positions arrive already in world space and the shader only applies viewProj.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(std140, set = 0, binding = 0) uniform SceneBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 material; // rgb = albedo, w = roughness
    vec4 params;   // x = light count, y = global intensity, z = 1 diffuse only, w = 1 two-sided
} ubo;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;

void main()
{
    outWorldPos = inPos;
    outNormal = inNormal;
    outUV = inUV;
    gl_Position = ubo.viewProj * vec4(inPos, 1.0);
}
