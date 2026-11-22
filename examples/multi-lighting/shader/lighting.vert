#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

layout(set = 3, binding = 0) readonly buffer ModelMatrices {
    mat4 models[];
} instance;

layout(push_constant) uniform ModelMatrix {
    mat4 model;
} modelMatrix;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) flat out int fragInstance;

void main() {
    mat4 M = instance.models[gl_InstanceIndex];
    vec4 world = M * vec4(inPosition, 1.0);
    gl_Position = camera.proj * camera.view * world;
    fragPos = world.xyz;
    fragNormal = mat3(transpose(inverse(M))) * inNormal;
    fragTexCoord = inTexCoord;
    fragInstance = gl_InstanceIndex;
}
