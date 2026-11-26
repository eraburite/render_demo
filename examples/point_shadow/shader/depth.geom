#version 450

// One draw, one cube face per invocation.  gl_InvocationID picks both the face
// matrix and the array layer, so the whole 6-face depth map is filled by a
// single instanced draw.  Same mechanism as csm/depth.geom, with 6 instead of 5.
//
// The invocation count is a SPIR-V execution mode fixed by the `invocations = 6`
// literal below.  It must equal the layer count in test31.cpp and nothing checks
// that agreement at run time.
layout(triangles, invocations = 6) in;
layout(triangle_strip, max_vertices = 3) out;

layout(std140, set = 0, binding = 0) uniform PointBlock
{
    mat4 faceMatrices[6];
    vec4 lightPosFar;  // xyz = light position, w = far plane
    mat4 viewProj;
    vec4 eye;
    vec4 flags;        // x = soft(0/1), y = shadow enable, z = debug, w = 1/resolution
} ubo;

layout(location = 0) in vec3 vWorldPos[];
layout(location = 0) out vec3 gWorldPos;

void main()
{
    for (int i = 0; i < 3; ++i) {
        gWorldPos = vWorldPos[i];
        gl_Position = ubo.faceMatrices[gl_InvocationID] * vec4(vWorldPos[i], 1.0);
        gl_Layer = gl_InvocationID;
        EmitVertex();
    }
    EndPrimitive();
}
