#version 450

// Pass-through tessellation control shader: every control point is copied
// unchanged and the tessellation levels come from a specialization constant.
// This is the "before" side of the comparison (flat, piecewise-linear mesh).
layout (constant_id = 1) const float uTessLevel = 6.0;

layout (vertices = 3) out;

layout (location = 0) in vec3 inNormal[];
layout (location = 0) out vec3 outNormal[3];

void main()
{
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = uTessLevel;
        gl_TessLevelOuter[1] = uTessLevel;
        gl_TessLevelOuter[2] = uTessLevel;
        gl_TessLevelInner[0] = uTessLevel;
    }

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    outNormal[gl_InvocationID] = inNormal[gl_InvocationID];
}
