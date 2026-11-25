#version 450

// Attributes match VertexInputAtlas {ePosition, eColor, eUV, eNormal}.
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inNormal;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;

void main()
{
    // Keep the control points in world space; the tessellation evaluation
    // shader applies viewProj after the bilinear sub-division.
    gl_Position = vec4(inPos, 1.0);
    outNormal = inNormal;
    outColor = inColor;
    outUV = inUV;
}
