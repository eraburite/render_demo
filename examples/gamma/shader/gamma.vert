#version 450

// Gamma-correction example: one instanced draw of a floor and four light
// markers.  The fragment shader selects between a linear and an sRGB view of
// the same texture, exactly like the two loadTexture() calls in 2.gamma.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

struct Instance {
    mat4 model;
    vec4 material; // rgb = albedo / marker colour, a > 0.5 = emissive marker
};
layout(std430, set = 2, binding = 0) readonly buffer Instances
{
    Instance instances[];
};

layout(std140, set = 0, binding = 0) uniform GammaBlock
{
    mat4 viewProj;
    vec4 eye;
    // x = gamma enabled, y/z/w unused
    vec4 params;
} ubo;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out vec4 outMaterial;

void main()
{
    const Instance instance = instances[gl_InstanceIndex];
    const vec4 world = instance.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;
    outNormal = transpose(inverse(mat3(instance.model))) * inNormal;
    outUV = inUV;
    outMaterial = instance.material;
    gl_Position = ubo.viewProj * world;
}
