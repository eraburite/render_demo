#version 450

// The array view comes from set0's TextureImageSpec (7 layers), and the shared
// layout declares the sampler for the fragment stage only.
layout(set = 0, binding = 0) uniform sampler2DArray samplerArray;

layout(location = 0) in vec3 inUV;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(samplerArray, inUV);
}
