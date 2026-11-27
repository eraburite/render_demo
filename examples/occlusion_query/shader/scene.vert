#version 450

// Sascha Willems' occlusionquery: the occluder is drawn first into the depth
// buffer, then each test object is drawn inside its own occlusion query.  The
// query counts samples that pass the depth test, so an object hidden behind the
// occluder reports zero (see DESIGN.md 28.2).
layout(set = 0, binding = 0) uniform CameraBlock
{
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform ObjectBlock
{
    mat4 model;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;

void main()
{
    gl_Position = camera.proj * camera.view * object.model * vec4(inPosition, 1.0);
    fragColor = inColor;
}
