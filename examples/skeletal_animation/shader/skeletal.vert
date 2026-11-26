#version 450

// Skinned variant of model_loading's vertex shader.
//
// The reference (8.guest/2020/skeletal_animation/anim_model.vs) declares seven
// attributes and computes a skinned normal that the fragment shader never reads.
// This port keeps only what the diffuse-only pass actually uses: position, UV,
// and the four bone influences.  Normals still exist in the loaded Model; they
// just have nowhere to go in a shader that writes texture() and nothing else.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in uvec4 inBoneIds;
layout(location = 3) in vec4 inBoneWeights;

layout(location = 0) out vec2 outTexCoord;

// std140 packs an array of mat4 as consecutive 16-byte-aligned columns, so this
// block matches the C++ SkeletonBlock byte for byte (3 mat4 + 64 mat4).
layout(set = 1, binding = 0) uniform SkeletonBlock {
    mat4 view;
    mat4 proj;
    mat4 model;
    mat4 joints[64];
} u;

void main()
{
    // Unused influence slots carry joint 0 with weight 0, so summing all four
    // unconditionally is correct and needs no -1 sentinel (the reference skips
    // boneIds[i] == -1 explicitly, which forces a signed attribute).
    vec4 skinned = vec4(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        const float weight = inBoneWeights[i];
        if (weight <= 0.0) {
            continue;
        }
        skinned += (u.joints[inBoneIds[i]] * vec4(inPosition, 1.0)) * weight;
        total += weight;
    }

    // A joint matrix is world(node) * inverseBind, and world(node) already
    // carries the file's own hierarchy scale (the vampire .dae puts 0.01 on its
    // root), so `skinned` arrives in render space.  `model` therefore only
    // carries this example's normalisation -- not the asset's own transform.
    const vec3 position = total > 0.0 ? skinned.xyz / total : inPosition;
    gl_Position = u.proj * u.view * u.model * vec4(position, 1.0);
    outTexCoord = inTexCoord;
}
