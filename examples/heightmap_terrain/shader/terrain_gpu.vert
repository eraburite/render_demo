#version 450

// GPU-displaced terrain, stage 1 of 4: the vertex shader is deliberately dumb.
// The control points are a flat lattice in the y = 0 plane carrying a heightmap
// UV; no height is known yet, so the only thing this stage does is forward the
// patch corners to the tessellation control shader.

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec2 inUV;

layout (location = 0) out vec3 outPos;
layout (location = 1) out vec2 outUV;

void main()
{
    // World space, unprojected: the evaluation shader applies the displacement
    // first and the transform afterwards.
    outPos = inPos;
    outUV = inUV;
    gl_Position = vec4(inPos, 1.0);
}
