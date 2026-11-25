#version 450

layout(set = 0, binding = 0) uniform sampler2D samplerColor;

// eViewEyePosition is declared for the fragment stage by the shared layout, so
// the eye position must be consumed here rather than forwarded from the vertex
// stage (a vertex-stage read would violate VUID-VkGraphicsPipelineCreateInfo-
// layout-07988).
layout(set = 2, binding = 0) uniform SceneBlock {
    vec4 eyePosition;
} scene;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in float inLodBias;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;

void main()
{
    // texture() with an explicit LOD walks the mip chain by hand: the example
    // keys change inLodBias so the difference between the baked levels is
    // visible without changing anything else in the frame.
    const vec4 albedo = texture(samplerColor, inTexCoord, inLodBias);

    const vec3 N = normalize(inNormal);
    const vec3 L = normalize(vec3(0.0, 0.0, 2.0) - inWorldPos);
    const vec3 V = normalize(scene.eyePosition.xyz - inWorldPos);
    const vec3 R = reflect(-L, N);

    const vec3 diffuse = max(dot(N, L), 0.0) * vec3(0.85, 0.88, 0.95);
    const float specular = pow(max(dot(R, V), 0.0), 32.0);

    outColor = vec4(diffuse * albedo.rgb + specular * vec3(1.0), 1.0);
}
