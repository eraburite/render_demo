#version 450

// 3x3 tent upsample, added on top of the matching downsample level.
//
// The reference expresses the addition with additive blending into the same
// attachment it already wrote during the downsample.  render_demo render passes
// own their attachments (a pass cannot reuse another pass' image as a colour
// target), so the sum is written explicitly into a separate target instead:
//
//     up[k] = mip[k] + tent(coarse[k])      coarse[4] = mip[5], else up[k+1]
//
// That is the same arithmetic -- base in set 1, coarse in set 2 -- without a
// load-op / blending dependency between two passes.
layout(set = 1, binding = 0) uniform sampler2D baseTexture;
layout(set = 2, binding = 0) uniform sampler2D coarseTexture;

layout(std140, set = 0, binding = 0) uniform MipBlock
{
    vec4 params; // xy = source resolution, z = filter radius (texture units), w = mip level
} ubo;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    // The kernel radius is expressed in texture coordinates, so it shrinks with
    // the mip: a constant 0.005 covers ~5 texels at 640x360 and ~1 at 20x11.
    const float x = ubo.params.z;
    const float y = ubo.params.z;

    const vec3 a = texture(coarseTexture, fragUV + vec2(-x, y)).rgb;
    const vec3 b = texture(coarseTexture, fragUV + vec2(0.0, y)).rgb;
    const vec3 c = texture(coarseTexture, fragUV + vec2(x, y)).rgb;

    const vec3 d = texture(coarseTexture, fragUV + vec2(-x, 0.0)).rgb;
    const vec3 e = texture(coarseTexture, fragUV).rgb;
    const vec3 f = texture(coarseTexture, fragUV + vec2(x, 0.0)).rgb;

    const vec3 g = texture(coarseTexture, fragUV + vec2(-x, -y)).rgb;
    const vec3 h = texture(coarseTexture, fragUV + vec2(0.0, -y)).rgb;
    const vec3 i = texture(coarseTexture, fragUV + vec2(x, -y)).rgb;

    // 1/16 * [1 2 1; 2 4 2; 1 2 1]
    vec3 tent = e * 4.0;
    tent += (b + d + f + h) * 2.0;
    tent += a + c + g + i;
    tent *= 1.0 / 16.0;

    const vec3 base = texture(baseTexture, fragUV).rgb;
    outColor = vec4(base + tent, 1.0);
}

