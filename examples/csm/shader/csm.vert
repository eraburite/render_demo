#version 450

// Main (lit) pass vertex shader.  The whole scene is one instanced draw: every
// box -- the ground slab included -- is a unit cube placed by an instance
// matrix, so the CPU never re-uploads geometry when the scene changes.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

struct Instance {
    mat4 model;
    vec4 material; // rgb = base colour, a > 0.5 => procedural checker
};

layout(std430, set = 1, binding = 0) readonly buffer Instances
{
    Instance instances[];
};

// Keep this block byte-identical to CsmUBO in test26.cpp (std140).
layout(std140, set = 0, binding = 0) uniform CsmBlock
{
    mat4 lightSpaceMatrices[5];
    mat4 view;
    mat4 viewProj;
    vec4 cascadePlaneDistances;
    vec4 lightDir;
    vec4 eye;
    vec4 flags;
} ubo;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) flat out vec4 outMaterial;

void main()
{
    const Instance inst = instances[gl_InstanceIndex];
    const vec4 world = inst.model * vec4(inPos, 1.0);

    outWorldPos = world.xyz;
    // The ground slab is scaled 140:1:140, so the plain mat3 would shear its
    // side-face normals into nonsense.  Inverse-transpose is the correct
    // transform for normals under non-uniform scale.
    outNormal = transpose(inverse(mat3(inst.model))) * inNormal;
    outMaterial = inst.material;

    gl_Position = ubo.viewProj * world;
}
