#version 450
// Subpass 1 (deferred composition): one oversized triangle, no vertex buffer.

layout(location = 0) out vec2 outUV;

void main()
{
    // (0,0) lands on the top-left of the attachment because Vulkan NDC has
    // y pointing down, which is exactly the gl_FragCoord orientation the input
    // attachments are read with.
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
