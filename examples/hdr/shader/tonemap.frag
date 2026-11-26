#version 450

// 6.hdr's tone mapping: exponential exposure followed by the 1/2.2 display
// encode.  Reinhard is kept as a third mode only as a diagnostic, matching the
// commented-out branch in the reference.
layout(set = 1, binding = 0) uniform sampler2D hdrColor;

layout(std140, set = 0, binding = 0) uniform ToneBlock
{
    // x = exposure, y = HDR enable (0 clips first), z = debug raw HDR, w unused
    vec4 params;
} ubo;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float GAMMA = 2.2;

void main()
{
    vec3 hdr = texture(hdrColor, fragUV).rgb;

    if (ubo.params.z > 0.5) {
        outColor = vec4(hdr / (vec3(1.0) + max(hdr, vec3(0.0))), 1.0);
        return;
    }

    // The reference's "HDR off" path is simply the same raw value with the
    // display encode applied; anything above 1 clips in the swapchain.
    if (ubo.params.y < 0.5) {
        hdr = clamp(hdr, 0.0, 1.0);
    } else {
        hdr = vec3(1.0) - exp(-hdr * ubo.params.x);
    }
    outColor = vec4(pow(max(hdr, vec3(0.0)), vec3(1.0 / GAMMA)), 1.0);
}
