#version 450

// Sascha Willems' instancing: binding 0 is the mesh (per-vertex), binding 1 is
// the instance stream (VK_VERTEX_INPUT_RATE_INSTANCE).  Locations 0..3 come
// from the mesh and 4..7 from the instance buffer; VertexInputAtlas assigns
// them contiguously in exactly that order (see DESIGN.md 28.3).
layout(set = 0, binding = 0) uniform CameraBlock
{
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;

layout(location = 4) in vec3 instPosition;
layout(location = 5) in vec3 instRotation;
layout(location = 6) in vec3 instScale;
layout(location = 7) in vec3 instColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;

mat3 rotationY(float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
}

void main()
{
    const mat3 rotation = rotationY(instRotation.y);
    const vec3 local = rotation * (inPosition * instScale);
    const vec3 world = instPosition + local;
    gl_Position = camera.proj * camera.view * vec4(world, 1.0);
    fragColor = inColor * instColor;
    fragNormal = rotation * inNormal;
}
