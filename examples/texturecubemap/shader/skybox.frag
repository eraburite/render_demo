#version 450

// The cube map lives in set0, which the shared layout declares for the fragment
// stage only, so the sampler is read here and nowhere else.
layout(set = 0, binding = 0) uniform samplerCube samplerCubeMap;

layout(location = 0) in vec3 inUVW;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(samplerCubeMap, inUVW);
}
