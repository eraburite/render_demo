#version 450

// Instanced sphere: one draw call over N instances, where the instance index
// selects both the per-instance model matrix (set3 SSBO) and the texture array
// layer (the point of the example).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

layout(set = 3, binding = 0) readonly buffer ModelMatrices {
    mat4 models[];
} instance;

layout(location = 0) out vec3 outUV;

void main()
{
    // gl_InstanceIndex is BOTH the transform slot and the array layer: there is
    // one matrix and one layer per drawn instance.
    const int layer = gl_InstanceIndex;

    outUV = vec3(inTexCoord, float(layer));
    gl_Position = camera.proj * camera.view * instance.models[layer] * vec4(inPosition, 1.0);
}
