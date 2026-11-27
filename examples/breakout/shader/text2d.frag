#version 450

// vec4(textColor, 1) * vec4(1, 1, 1, coverage) multiplies out to
// (textColor, coverage); only the alpha channel does anything, which is exactly
// what (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) blending needs.
layout(set = 1, binding = 0) uniform sampler2D atlasTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(fragColor, texture(atlasTexture, fragUV).r);
}
