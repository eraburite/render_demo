#version 450

// Locations follow the VertexInputAtlas component order used by test41:
//   0 position, 1 normal, 2 uv, 3 colour.
layout(set = 0, binding = 0) uniform CameraBlock
{
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inColor;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;

mat3 rotY(float a)
{
    const float c = cos(a), s = sin(a);
    return mat3(c, 0.0, -s, 0.0, 1.0, 0.0, s, 0.0, c);
}

mat3 rotX(float a)
{
    const float c = cos(a), s = sin(a);
    return mat3(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);
}

void main()
{
    // A fixed tilt so the cube reads as 3D in a single dump; deterministic,
    // and it applies the same rotation to the normal for the lighting term.
    const mat3 tilt = rotX(0.35) * rotY(0.60);
    const vec3 position = tilt * inPosition;
    gl_Position = camera.proj * camera.view * vec4(position, 1.0);
    fragColor = inColor;
    fragNormal = tilt * inNormal;
    fragUV = inUV;
}
