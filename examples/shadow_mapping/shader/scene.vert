#version 450

// Single-shadow-map example: the whole scene is one instanced draw.  Instance 0
// is the ground slab, the rest are cubes, so neither pass needs a model matrix
// push constant or a CPU-side geometry rebuild.
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

// Byte-identical to ShadowUBO in test30.cpp (std140).  The depth pass reads
// lightSpaceMatrix; the lit pass reads viewProj/lightDir/eye/flags.
layout(std140, set = 0, binding = 0) uniform ShadowBlock
{
    mat4 lightSpaceMatrix;
    mat4 viewProj;
    vec4 lightDir;  // xyz = direction towards the light
    vec4 eye;
    vec4 flags;     // x = debug view, y = bias, z = PCF, w = shadow enable
} ubo;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) flat out vec4 outMaterial;

void main()
{
    const Instance inst = instances[gl_InstanceIndex];
    const vec4 world = inst.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;
    // The ground slab is scaled 25:0.1:25, so the plain mat3 would shear its
    // normals into nonsense.  Inverse-transpose is the correct transform.
    outNormal = transpose(inverse(mat3(inst.model))) * inNormal;
    outMaterial = inst.material;
    gl_Position = ubo.viewProj * world;
}
