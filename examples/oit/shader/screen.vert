#version 450

// Fullscreen quad for the screen pass.  Positions arrive in NDC already, so the
// vertex stage only forwards them.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 outUV;

void main()
{
    outUV = inUV;
    gl_Position = vec4(inPosition, 1.0);
}
