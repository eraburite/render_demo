#version 450

// Point-shadow depth pass vertex stage.  Like csm's depth shader it only does
// the model transform and hands a WORLD-SPACE position to the geometry stage:
// the GS applies a different cube-face matrix per invocation and cannot undo a
// projection that was baked in here.
layout(location = 0) in vec3 inPos;

struct Instance {
    mat4 model;
    vec4 material;
};
layout(std430, set = 1, binding = 0) readonly buffer Instances
{
    Instance instances[];
};

layout(location = 0) out vec3 vWorldPos;

void main()
{
    vWorldPos = (instances[gl_InstanceIndex].model * vec4(inPos, 1.0)).xyz;
    gl_Position = vec4(vWorldPos, 1.0);
}
