#version 450

// Reflective object: forwards the world position/normal and the reference's
// fixed light and view vectors to the fragment stage, where the reflection
// vector is built and the cube map is sampled.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform PushBlock {
    mat4 model;
    float lodBias;
} push;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outViewVec;
layout(location = 3) out vec3 outLightVec;

void main()
{
    const vec4 world = push.model * vec4(inPosition, 1.0);
    gl_Position = camera.proj * camera.view * world;

    outWorldPos = world.xyz;
    outNormal = mat3(push.model) * inNormal;

    // Same fixed light as the reference example.
    const vec3 lightPos = vec3(0.0, -5.0, 5.0);
    outLightVec = lightPos - outWorldPos;
    outViewVec = -outWorldPos;
}
