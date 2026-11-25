#version 450
// Subpass 2 (forward transparency).  Geometry is pre-transformed into world
// space on the CPU, so only the view-projection is applied.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;

layout(binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) out vec3 outColor;
layout(location = 1) out vec2 outUV;

void main()
{
    outColor = inColor;
    outUV = inUV;
    gl_Position = camera.proj * camera.view * vec4(inPos, 1.0);
}
