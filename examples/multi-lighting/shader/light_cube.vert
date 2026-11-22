#version 450
layout(location = 0) in vec3 inPosition;
layout(set = 1, binding = 0) uniform CameraMatrices { mat4 view; mat4 proj; } camera;
layout(set = 3, binding = 0) readonly buffer ModelMatrices { mat4 models[]; } instance;
layout(location = 0) out vec3 fragColor;
void main() {
    gl_Position = camera.proj * camera.view * instance.models[gl_InstanceIndex] * vec4(inPosition * 0.25, 1.0);
    fragColor = vec3(1.0);
}
