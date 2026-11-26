#version 450

// 3.1.1's debug depth quad: the light-space depth is orthographic, so it is
// already linear and needs no near/far linearisation (the reference leaves
// LinearizeDepth() in the source but never calls it).
layout(location = 0) in vec2 fragTexCoord;
layout(set = 0, binding = 0) uniform sampler2D depthMap;
layout(location = 0) out vec4 outColor;

void main()
{
    const float depth = texture(depthMap, fragTexCoord).r;
    outColor = vec4(vec3(depth), 1.0);
}
