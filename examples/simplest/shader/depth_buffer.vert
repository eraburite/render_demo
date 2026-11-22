#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform ModelMatrix {
    mat4 model;
} modelMatrix;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = camera.proj * camera.view * modelMatrix.model * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
