#version 450

// Vertical half of the 9-tap Gaussian; identical to blur_h except for the
// sampling axis.  Two shader files keep the direction a compile-time constant.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float WEIGHT[5] = float[5](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);

void main()
{
    const vec2 texel = 1.0 / vec2(textureSize(source, 0));
    vec3 result = texture(source, fragUV).rgb * WEIGHT[0];
    for (int i = 1; i < 5; ++i) {
        const vec2 offset = vec2(0.0, texel.y * float(i));
        result += texture(source, fragUV + offset).rgb * WEIGHT[i];
        result += texture(source, fragUV - offset).rgb * WEIGHT[i];
    }
    outColor = vec4(result, 1.0);
}
