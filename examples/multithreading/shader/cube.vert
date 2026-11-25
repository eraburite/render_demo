#version 450

// Attributes matching VertexInputAtlas {ePosition, eColor, eUV, eNormal}.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;

// set1 binding0: camera view/proj (shared by all thread groups).
layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

// set3 binding0: this group's model matrices, indexed by gl_InstanceIndex.
layout(set = 3, binding = 0) readonly buffer ModelMatrices {
    mat4 models[];
} instance;

// Per-thread tint, pushed once by each secondary before its draw.
layout(push_constant) uniform ThreadTint {
    vec4 color;
} tint;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec4 outColor;

void main()
{
    mat4 model = instance.models[gl_InstanceIndex];
    vec4 world = model * vec4(inPos, 1.0);
    gl_Position = camera.proj * camera.view * world;
    // Instances use uniform scale only, so the upper 3x3 is enough for normals.
    outNormal = mat3(model) * inNormal;
    outColor = vec4(inColor, 1.0) * tint.color;
}
