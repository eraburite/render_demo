#version 450

// Background cube: the model matrix only scales/centres the cube on the camera,
// so the object-space position doubles as the sampling direction.  xy is
// flipped to convert GL cube coordinates into Vulkan's.

layout(location = 0) in vec3 inPosition;

layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform PushBlock {
    mat4 model;
    float lodBias;
} push;

layout(location = 0) out vec3 outUVW;

void main()
{
    outUVW = inPosition;
    outUVW.xy *= -1.0;

    gl_Position = camera.proj * camera.view * push.model * vec4(inPosition, 1.0);
}
