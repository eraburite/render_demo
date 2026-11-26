#version 450

// Depth-only pass: project world positions into the light's orthographic
// frustum.  No geometry shader and no gl_Layer -- this is the single 2D depth
// texture of LearnOpenGL 3.1.x, not the 5-layer array from examples/csm.
layout(location = 0) in vec3 inPos;

struct Instance {
    mat4 model;
    vec4 material;
};

layout(std430, set = 1, binding = 0) readonly buffer Instances
{
    Instance instances[];
};

layout(std140, set = 0, binding = 0) uniform ShadowBlock
{
    mat4 lightSpaceMatrix;
    mat4 viewProj;
    vec4 lightDir;
    vec4 eye;
    vec4 flags;
} ubo;

void main()
{
    gl_Position = ubo.lightSpaceMatrix * instances[gl_InstanceIndex].model * vec4(inPos, 1.0);
}
