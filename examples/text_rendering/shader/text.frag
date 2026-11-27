#version 450

// The reference writes
//     vec4 sampled = vec4(1, 1, 1, texture(text, TexCoords).r);
//     color = vec4(textColor, 1) * sampled;
// which multiplies out to (textColor, coverage).  There is no need to spell the
// multiply out: only the alpha channel is non-trivial, and with (SRC_ALPHA,
// ONE_MINUS_SRC_ALPHA) blending that alpha is exactly what makes the glyph edge
// soft instead of a coloured box.
//
// Vulkan returns (r, 0, 0, 1) for an R8 view, so .r is the coverage; GL's
// GL_RED texture returns the same value, which is why the reference can use .r
// unchanged.
layout(set = 1, binding = 0) uniform sampler2D atlasTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    const float coverage = texture(atlasTexture, fragUV).r;
    outColor = vec4(fragColor, coverage);
}
