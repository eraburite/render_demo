#version 450

// The reference's "old" bloom: five horizontal + five vertical 9-tap Gaussian
// passes over a full-resolution ping-pong pair (programChoice == 2).  Kept in
// this example so the two approaches can be compared on identical geometry.
layout(set = 1, binding = 0) uniform sampler2D srcTexture;

layout(std140, set = 0, binding = 0) uniform MipBlock
{
    vec4 params; // x = 1 horizontal, 0 vertical; yzw unused
} ubo;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float WEIGHT[5] = float[5](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

void main()
{
    const vec2 texel = 1.0 / vec2(textureSize(srcTexture, 0));
    const vec2 direction = ubo.params.x > 0.5 ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec3 result = texture(srcTexture, fragUV).rgb * WEIGHT[0];
    for (int i = 1; i < 5; ++i) {
        const vec2 offset = direction * float(i);
        result += texture(srcTexture, fragUV + offset).rgb * WEIGHT[i];
        result += texture(srcTexture, fragUV - offset).rgb * WEIGHT[i];
    }
    outColor = vec4(result, 1.0);
}
