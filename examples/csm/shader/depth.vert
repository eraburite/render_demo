#version 450

// Shadow pass vertex shader.  It does the model transform only and hands the
// result to the geometry shader as a *world-space* position (w = 1), because
// the geometry shader has to apply a different light matrix per invocation and
// cannot undo a projection that has already been baked in.
layout(location = 0) in vec3 inPos;

struct Instance {
    mat4 model;
    vec4 material;
};

layout(std430, set = 1, binding = 0) readonly buffer Instances
{
    Instance instances[];
};

void main()
{
    gl_Position = instances[gl_InstanceIndex].model * vec4(inPos, 1.0);
}
