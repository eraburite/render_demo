#version 450

// Vertex attributes follow VertexInputAtlas {ePosition, eNormal}:
//   location 0 = position, location 1 = normal.
//
// The control points stay in model space.  The tessellation evaluation stage
// applies viewProj and remaps clip-space X into the left/right half of the
// screen, so the same vertex/index buffers feed both comparison pipelines.
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;

layout (location = 0) out vec3 outNormal;

void main()
{
    gl_Position = vec4(inPos, 1.0);
    outNormal = inNormal;
}
