#version 450

// 13-tap downsample from the COD/SIGGRAPH 2014 bloom-filtering talk, as used by
// 6.physically_based_bloom.  Level 0 additionally applies a Karis average to
// each 2x2 block: without it a single very bright texel makes the whole mip
// flicker as the camera moves ("fireflies", pulsating artifacts).
//
// Note the weights are the *distributed* ones: the sum of the five 2x2 groups
// is 0.125*4 + 0.5 = 1, so the filter is energy preserving.
layout(set = 1, binding = 0) uniform sampler2D srcTexture;

layout(std140, set = 0, binding = 0) uniform MipBlock
{
    vec4 params; // xy = source resolution, z = filter radius, w = mip level being written
} ubo;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

vec3 toSRGB(vec3 v)
{
    return pow(max(v, vec3(0.0)), vec3(1.0 / 2.2));
}

// The reference uses the Rec.601 luma here even though the threshold uses
// Rec.709; kept as-is so the mip chain matches texel for texel.
float karisAverage(vec3 col)
{
    const float luma = dot(toSRGB(col), vec3(0.299, 0.587, 0.114)) * 0.25;
    return 1.0 / (1.0 + luma);
}

void main()
{
    const vec2 texelSize = 1.0 / ubo.params.xy;
    const float x = texelSize.x;
    const float y = texelSize.y;

    // a b c
    // j e k   (e is the current texel)
    // l m n
    const vec3 a = texture(srcTexture, fragUV + vec2(-2.0 * x, 2.0 * y)).rgb;
    const vec3 b = texture(srcTexture, fragUV + vec2(0.0, 2.0 * y)).rgb;
    const vec3 c = texture(srcTexture, fragUV + vec2(2.0 * x, 2.0 * y)).rgb;

    const vec3 d = texture(srcTexture, fragUV + vec2(-2.0 * x, 0.0)).rgb;
    const vec3 e = texture(srcTexture, fragUV).rgb;
    const vec3 f = texture(srcTexture, fragUV + vec2(2.0 * x, 0.0)).rgb;

    const vec3 g = texture(srcTexture, fragUV + vec2(-2.0 * x, -2.0 * y)).rgb;
    const vec3 h = texture(srcTexture, fragUV + vec2(0.0, -2.0 * y)).rgb;
    const vec3 i = texture(srcTexture, fragUV + vec2(2.0 * x, -2.0 * y)).rgb;

    const vec3 j = texture(srcTexture, fragUV + vec2(-x, y)).rgb;
    const vec3 k = texture(srcTexture, fragUV + vec2(x, y)).rgb;
    const vec3 l = texture(srcTexture, fragUV + vec2(-x, -y)).rgb;
    const vec3 m = texture(srcTexture, fragUV + vec2(x, -y)).rgb;

    vec3 result;
    if (ubo.params.w < 0.5) {
        // Writing mip 0: Karis-average each of the five 2x2 blocks.
        vec3 groups[5];
        groups[0] = (a + b + d + e) * (0.125 / 4.0);
        groups[1] = (b + c + e + f) * (0.125 / 4.0);
        groups[2] = (d + e + g + h) * (0.125 / 4.0);
        groups[3] = (e + f + h + i) * (0.125 / 4.0);
        groups[4] = (j + k + l + m) * (0.5 / 4.0);
        for (int n = 0; n < 5; ++n) {
            groups[n] *= karisAverage(groups[n]);
        }
        result = groups[0] + groups[1] + groups[2] + groups[3] + groups[4];
        // A zero mip would make the following upsample multiply by zero.
        result = max(result, vec3(0.0001));
    } else {
        result = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }
    outColor = vec4(result, 1.0);
}
