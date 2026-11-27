#version 450

// Final tone map of 6.physically_based_bloom.  The three modes are exactly the
// reference's programChoice branches:
//   0  bloom off            hdr
//   1  old Gaussian bloom   hdr + bloom            (additive)
//   2  mip-chain bloom      mix(hdr, bloom, 0.04)  (physically based)
// Mode 2 *replaces* most of the scene with the bloom buffer, which only looks
// right because the mip chain carries the whole scene, not just a threshold.
layout(set = 1, binding = 0) uniform sampler2D sceneTexture;
layout(set = 2, binding = 0) uniform sampler2D bloomTexture;

layout(std140, set = 0, binding = 0) uniform CompositeBlock
{
    vec4 params; // x = exposure, y = mode, z = bloom strength, w = debug bloom only
} ubo;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float GAMMA = 2.2;

void main()
{
    const vec3 hdr = texture(sceneTexture, fragUV).rgb;
    const vec3 bloom = texture(bloomTexture, fragUV).rgb;

    vec3 result = hdr;
    const int mode = int(ubo.params.y + 0.5);
    if (ubo.params.w > 0.5) {
        result = bloom;
    } else if (mode == 1) {
        result = hdr + bloom;
    } else if (mode == 2) {
        result = mix(hdr, bloom, ubo.params.z);
    }

    result = vec3(1.0) - exp(-result * ubo.params.x);
    outColor = vec4(pow(max(result, vec3(0.0)), vec3(1.0 / GAMMA)), 1.0);
}
