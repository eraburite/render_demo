#version 450

// Displays the compute output.  The image stays in GENERAL (it is both a storage
// image and a sampled texture), so the descriptor -- not a per-frame barrier --
// carries the layout.
layout(set = 0, binding = 0) uniform sampler2D sceneImage;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(sceneImage, fragUV);
}
