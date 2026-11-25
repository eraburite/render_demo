#version 450
// Subpass 2 (forward transparency): samples the glass texture and rejects
// fragments that the opaque pass already covered, using the linear depth that
// subpass 0 packed into the position attachment's alpha channel.

layout(input_attachment_index = 0, binding = 1) uniform subpassInput samplerPositionDepth;
layout(binding = 2) uniform sampler2D samplerTexture;

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// Must match MakeCameraMatrix() in examples/common/RenderCommon.h.
const float NEAR_PLANE = 0.1;
const float FAR_PLANE = 100.0;

float linearDepth(float depth)
{
    float z = depth * 2.0 - 1.0;
    return (2.0 * NEAR_PLANE * FAR_PLANE) / (FAR_PLANE + NEAR_PLANE - z * (FAR_PLANE - NEAR_PLANE));
}

void main()
{
    float depth = subpassLoad(samplerPositionDepth).a;

    // Sample before the discard: texture() needs implicit derivatives, and those
    // are undefined inside non-uniform control flow.
    vec4 sampledColor = texture(samplerTexture, inUV);
    if (depth != 0.0 && linearDepth(gl_FragCoord.z) > depth)
    {
        discard;
    }

    outColor = sampledColor * vec4(inColor, 1.0);
}
