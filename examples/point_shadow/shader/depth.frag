#version 450

// Unlike a directional light, a point light stores RADIAL DISTANCE / far_plane,
// not the projected NDC depth.  The reference writes gl_FragDepth directly for
// exactly this reason: the comparison in main() is length(fragToLight)/far too.
// Writing a custom depth disables early-z, which is irrelevant for a 1024^2 map.
layout(std140, set = 0, binding = 0) uniform PointBlock
{
    mat4 faceMatrices[6];
    vec4 lightPosFar;
    mat4 viewProj;
    vec4 eye;
    vec4 flags;
} ubo;

layout(location = 0) in vec3 gWorldPos;

void main()
{
    const float distance = length(gWorldPos - ubo.lightPosFar.xyz) / ubo.lightPosFar.w;
    gl_FragDepth = clamp(distance, 0.0, 1.0);
}
