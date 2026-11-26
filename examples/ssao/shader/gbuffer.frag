#version 450

layout(location = 0) in vec3 inViewPos;
layout(location = 1) in vec3 inViewNormal;
layout(location = 2) in vec3 inAlbedo;

layout(location = 0) out vec4 outPosition;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outAlbedo;

void main()
{
    // Alpha 1 marks covered texels; the pass clears to alpha 0 so the SSAO and
    // lighting passes can skip empty fragments and avoid normalize(vec3(0)).
    outPosition = vec4(inViewPos, 1.0);
    outNormal = vec4(normalize(inViewNormal), 1.0);
    outAlbedo = vec4(inAlbedo, 1.0);
}
