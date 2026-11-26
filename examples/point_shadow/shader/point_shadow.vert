#version 450

// Lit pass, one instanced draw for the whole scene (same as csm/csm.vert).
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

layout(std140, set = 0, binding = 0) uniform PointBlock
{
    mat4 faceMatrices[6];
    vec4 lightPosFar;
    mat4 viewProj;
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
    outNormal = transpose(inverse(mat3(inst.model))) * inNormal;
    outMaterial = inst.material;
    gl_Position = ubo.viewProj * world;
}
