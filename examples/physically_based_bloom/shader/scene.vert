#version 450

// Geometry pass for 7.bloom.  One instanced draw writes both the scene radiance
// and the thresholded bright pass into two RGBA16F attachments (MRT).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

struct Instance {
    mat4 model;
    vec4 material; // rgb = colour / radiance, a > 0.5 selects emissive
};
layout(std430, set = 1, binding = 0) readonly buffer Instances
{
    Instance instances[];
};

layout(std140, set = 0, binding = 0) uniform BloomBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 params; // x = bloom enable, y = debug bright buffer, z/w unused
} ubo;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) flat out vec4 outMaterial;

void main()
{
    const Instance inst = instances[gl_InstanceIndex];
    const vec4 world = inst.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;
    outNormal = transpose(inverse(mat3(inst.model))) * inNormal;
    outMaterial = inst.material;
    gl_Position = ubo.viewProj * world;
}
