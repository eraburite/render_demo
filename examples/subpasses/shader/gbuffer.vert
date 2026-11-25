#version 450
// Subpass 0 (G-Buffer fill).  Geometry is pre-transformed into world space on
// the CPU, so this only applies the view-projection; the world position and
// normal are forwarded for the deferred pass.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;

layout(binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;
layout(location = 2) out vec3 outWorldPos;

void main()
{
    gl_Position = camera.proj * camera.view * vec4(inPos, 1.0);
    outWorldPos = inPos;
    // No model matrix: pre-transform already applied its inverse-transpose.
    outNormal = normalize(inNormal);
    outColor = inColor;
}
