#version 450

// 3D texture volume-rendering vertex stage.  The reference (texture3d.vert)
// just forwards the interpolated slice coordinate; this version hands the raw
// object-space position to the fragment stage so the ray marcher can build the
// view ray in the volume's own space.

layout(location = 0) in vec3 inPosition;

layout(set = 1, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform PushBlock {
    mat4 model;
    float densityGain;
    float scroll;      // animated offset into the volume
    float time;
    int steps;
} push;

layout(location = 0) out vec3 outObjectPos;

void main()
{
    gl_Position = camera.proj * camera.view * push.model * vec4(inPosition, 1.0);
    outObjectPos = inPosition;
}
