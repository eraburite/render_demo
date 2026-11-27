#version 450

// Reference: color = particleColor * texture(sprite, TexCoords), drawn with
// glBlendFunc(GL_SRC_ALPHA, GL_ONE) -- additive, which is what turns the
// trail into a glow instead of a stack of squares.
layout(set = 2, binding = 0) uniform sampler2D spriteTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = fragColor * texture(spriteTexture, fragUV);
}
