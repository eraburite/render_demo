#version 450

// GPU-displaced terrain, stage 2 of 4: tessellation control.
//
// The only job here is choosing how finely each patch is subdivided.  The
// decision is per *edge*, not per patch: an edge's level comes from the distance
// of its two end points to the camera, so neighbouring patches agree on the
// level of the edge they share and the mesh stays watertight without any
// CPU-side stitching.
//
// Control point order is the tessellator's: 0, 1, 2, 3 run around the perimeter,
// with u along 0->1 and v along 0->3.

layout (set = 1, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    mat4 view;
    vec4 eye;     // xyz: camera position in world space
    vec4 height;  // x: height span, y: height shift, z: grayscale flag
    vec4 terrain; // x: world width, y: world depth, z: one texel in u, w: one texel in v
    vec4 tess;    // x: min level, y: max level, z: min distance, w: max distance
} ubo;

layout (vertices = 4) out;

layout (location = 0) in vec3 inPos[];
layout (location = 1) in vec2 inUV[];

layout (location = 0) out vec3 outPos[4];
layout (location = 1) out vec2 outUV[4];

// 0 near the camera (=> dense), 1 far away (=> coarse).
float distanceFactor(vec4 viewA, vec4 viewB)
{
    // Depth along the view axis, which is what the eye actually resolves; the
    // patch is small enough that the difference to the true radial distance is
    // not worth a sqrt per edge.
    float depth = min(abs(viewA.z), abs(viewB.z));
    float span = max(ubo.tess.w - ubo.tess.z, 1.0);
    return clamp((depth - ubo.tess.z) / span, 0.0, 1.0);
}

void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    outPos[gl_InvocationID] = inPos[gl_InvocationID];
    outUV[gl_InvocationID] = inUV[gl_InvocationID];

    if (gl_InvocationID == 0) {
        vec4 v0 = ubo.view * gl_in[0].gl_Position;
        vec4 v1 = ubo.view * gl_in[1].gl_Position;
        vec4 v2 = ubo.view * gl_in[2].gl_Position;
        vec4 v3 = ubo.view * gl_in[3].gl_Position;

        // Outer edge i connects control point i to i+1, so the shared edge of
        // two adjacent patches gets the same factor from both sides.
        float f0 = distanceFactor(v0, v1);
        float f1 = distanceFactor(v1, v2);
        float f2 = distanceFactor(v2, v3);
        float f3 = distanceFactor(v3, v0);

        gl_TessLevelOuter[0] = mix(ubo.tess.y, ubo.tess.x, f0);
        gl_TessLevelOuter[1] = mix(ubo.tess.y, ubo.tess.x, f1);
        gl_TessLevelOuter[2] = mix(ubo.tess.y, ubo.tess.x, f2);
        gl_TessLevelOuter[3] = mix(ubo.tess.y, ubo.tess.x, f3);

        // Inner levels follow the u-parallel and v-parallel edge pairs, so the
        // interior density matches the border it connects to.
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);
        gl_TessLevelInner[1] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
    }
}
