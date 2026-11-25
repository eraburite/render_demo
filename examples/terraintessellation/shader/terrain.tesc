#version 450

layout (set = 0, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    vec4 eye;        // xyz: camera position in world space
    vec4 tessParams; // x: near dist, y: far dist, z: min level, w: max level
} ubo;

layout (vertices = 4) out;

layout (location = 0) in vec3 inNormal[];
layout (location = 1) in vec3 inColor[];
layout (location = 2) in vec2 inUV[];

layout (location = 0) out vec3 outNormal[4];
layout (location = 1) out vec3 outColor[4];
layout (location = 2) out vec2 outUV[4];

void main()
{
    if (gl_InvocationID == 0) {
        // Dynamic LOD: dense near the camera, coarse far away.
        vec3 center = (gl_in[0].gl_Position.xyz + gl_in[1].gl_Position.xyz
                       + gl_in[2].gl_Position.xyz + gl_in[3].gl_Position.xyz) * 0.25;
        float dist = distance(center, ubo.eye.xyz);
        float span = max(ubo.tessParams.y - ubo.tessParams.x, 0.001);
        float t = clamp((dist - ubo.tessParams.x) / span, 0.0, 1.0);
        float level = clamp(mix(ubo.tessParams.w, ubo.tessParams.z, t), 1.0, 16.0);

        gl_TessLevelOuter[0] = level;
        gl_TessLevelOuter[1] = level;
        gl_TessLevelOuter[2] = level;
        gl_TessLevelOuter[3] = level;
        gl_TessLevelInner[0] = level;
        gl_TessLevelInner[1] = level;
    }

    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    outNormal[gl_InvocationID] = inNormal[gl_InvocationID];
    outColor[gl_InvocationID] = inColor[gl_InvocationID];
    outUV[gl_InvocationID] = inUV[gl_InvocationID];
}
