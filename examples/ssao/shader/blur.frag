#version 450

// 4x4 box blur of the raw AO buffer (9.ssao_blur.fs).  The reference's loop runs
// x,y in [-2,2) and divides by 16; it is a plain box filter with no bilateral
// depth weighting, so it only removes the noise-texture banding.
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D ssaoInput;

void main()
{
    const vec2 texelSize = 1.0 / vec2(textureSize(ssaoInput, 0));
    float result = 0.0;
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            result += texture(ssaoInput, inUV + vec2(float(x), float(y)) * texelSize).r;
        }
    }
    const float ao = result / 16.0;
    outColor = vec4(vec3(ao), 1.0);
}
