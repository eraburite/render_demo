#version 450

// Shared vertex stage of the scene-graph example.  Every entity is the same
// planet mesh, so the per-entity state is exactly one mat4 and gl_InstanceIndex
// selects it from a table.  Frustum culling then becomes "how many entries does
// the table hold this frame", not "which command buffers do I record".

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;

// InstanceMatrices in examples/common/DescriptorLayoutConfig.h (set3).
layout(set = 3, binding = 0) readonly buffer InstanceMatrices {
    mat4 models[];
} instance;

layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outWorldPos;

void main()
{
    const mat4 model = instance.models[gl_InstanceIndex];
    const vec4 worldPosition = model * vec4(inPosition, 1.0);
    outWorldPos = worldPosition.xyz;
    // Entity transforms are translate+rotate+uniform-scale, so the linear part
    // is a similarity transform and the inverse-transpose is the same matrix up
    // to a scalar.  Normalising in the fragment stage absorbs that scalar.
    outNormal = mat3(model) * inNormal;
    outUV = inUV;
    gl_Position = camera.proj * camera.view * worldPosition;
}
