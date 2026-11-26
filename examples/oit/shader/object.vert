#version 450

// Shared vertex stage of the two OIT geometry passes (solid and transparent).
// The three quads live in one static SSBO; gl_InstanceIndex selects the entry,
// so the solid pass draws instance 0 and the transparent pass draws 1..2 with a
// plain instanced draw instead of three separate draws with per-draw uniforms.

layout(location = 0) in vec3 inPosition;

// OitObject in examples/common/DescriptorLayoutConfig.h.
struct OitObject {
    mat4 model;
    vec4 color;
};

layout(set = 0, binding = 0) readonly buffer ObjectBlock {
    OitObject objects[];
} objectBlock;

layout(set = 1, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) out vec4 outColor;

void main()
{
    const OitObject object = objectBlock.objects[gl_InstanceIndex];
    outColor = object.color;
    gl_Position = camera.proj * camera.view * object.model * vec4(inPosition, 1.0);
}
