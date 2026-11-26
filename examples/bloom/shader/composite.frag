#version 450

// 7.bloom's final pass: scene + blurred bright buffer, exponential exposure,
// then the 1/2.2 display encode.  The B key in the reference only skips the
// addition; tone mapping stays active either way.
layout(set = 1, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 0) uniform sampler2D bloomColor;

layout(std140, set = 0, binding = 0) uniform CompositeBlock
{
    // x = exposure, y = bloom enable, z = debug bloom buffer, w unused
    vec4 params;
} ubo;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float GAMMA = 2.2;

void main()
{
    const vec3 scene = texture(sceneColor, fragUV).rgb;
    const vec3 bloom = texture(bloomColor, fragUV).rgb;
    if (ubo.params.z > 0.5) {
        outColor = vec4(bloom / (vec3(1.0) + max(bloom, vec3(0.0))), 1.0);
        return;
    }
    vec3 hdr = scene + (ubo.params.y > 0.5 ? bloom : vec3(0.0));
    hdr = vec3(1.0) - exp(-hdr * ubo.params.x);
    outColor = vec4(pow(max(hdr, vec3(0.0)), vec3(1.0 / GAMMA)), 1.0);
}
