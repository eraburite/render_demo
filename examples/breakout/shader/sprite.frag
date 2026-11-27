#version 450

// Reference: color = vec4(spriteColor, 1.0) * texture(sprite, TexCoords).
// The tint is applied to rgb only and alpha comes from the texture, which is
// what makes the ball, paddle and power-up pngs keep their soft edges.
layout(set = 2, binding = 0) uniform sampler2D spriteTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(fragColor.rgb, 1.0) * texture(spriteTexture, fragUV);
}
