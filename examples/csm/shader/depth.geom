#version 450

// One draw call, one cascade per invocation.  gl_InvocationID picks both the
// light matrix and the array layer, so the whole 5-layer shadow map is filled
// by a single instanced draw instead of five passes -- which is exactly what the
// reference does, and the only way to get the same depth resolution per cascade
// without re-recording the scene five times.
//
// The invocation count is a SPIR-V execution mode, not a pipeline state: it is
// fixed by this `invocations = 5` literal (and emitted into the SPIR-V header by
// glslang).  It must equal kMaxCascades in test26.cpp and the array size of
// lightSpaceMatrices above; nothing checks that agreement at run time.
layout(triangles, invocations = 5) in;
layout(triangle_strip, max_vertices = 3) out;

layout(std140, set = 0, binding = 0) uniform CsmBlock
{
    mat4 lightSpaceMatrices[5];
    mat4 view;
    mat4 viewProj;
    vec4 cascadePlaneDistances;
    vec4 lightDir;
    vec4 eye;
    vec4 flags;
} ubo;

void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = ubo.lightSpaceMatrices[gl_InvocationID] * gl_in[i].gl_Position;
        gl_Layer = gl_InvocationID;
        EmitVertex();
    }
    EndPrimitive();
}
