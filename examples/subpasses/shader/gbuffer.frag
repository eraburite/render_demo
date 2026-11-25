#version 450
// Subpass 0 (G-Buffer fill): writes four colour targets.  Attachment 0 is the
// swapchain image; it is cleared here as well because every colour attachment
// of a subpass has to be written (consistent framebuffer / undefined values on
// tile-based GPUs).

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;    // swapchain (throw-away)
layout(location = 1) out vec4 outPosition; // xyz world position, a = linear depth
layout(location = 2) out vec4 outNormal;   // xyz world normal
layout(location = 3) out vec4 outAlbedo;   // rgb albedo

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
    outColor = vec4(0.0);
    // Linear depth in alpha lets the transparent subpass compare distances
    // without access to the depth attachment.
    outPosition = vec4(inWorldPos, linearDepth(gl_FragCoord.z));
    outNormal = vec4(normalize(inNormal), 1.0);
    outAlbedo = vec4(inColor, 1.0);
}
