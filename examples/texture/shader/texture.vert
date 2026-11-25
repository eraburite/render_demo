#version 450

// Textured quad with a mip-mapped KTX texture.  The LOD bias arrives through the
// push constant block so the example can walk the mip chain with +/- without
// touching the descriptor set.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;

layout(set = 1, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform PushBlock {
    mat4 model;
    float lodBias;
} push;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out float outLodBias;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec3 outWorldPos;

void main()
{
    const vec3 worldPos = vec3(push.model * vec4(inPosition, 1.0));

    gl_Position = camera.proj * camera.view * push.model * vec4(inPosition, 1.0);

    outTexCoord = inTexCoord;
    outLodBias = push.lodBias;
    // The quad is flat and unrotated, so the model matrix cannot skew normals;
    // a full inverse-transpose would only add cost here.
    outNormal = inNormal;
    outWorldPos = worldPos;
}
