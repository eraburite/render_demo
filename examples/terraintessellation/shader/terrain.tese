#version 450

layout (set = 0, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    vec4 eye;
    vec4 tessParams;
} ubo;

layout (quads, equal_spacing, cw) in;

layout (location = 0) in vec3 inNormal[];
layout (location = 1) in vec3 inColor[];
layout (location = 2) in vec2 inUV[];

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec3 outWorldPos;
layout (location = 4) out vec3 outViewVec;

void main()
{
    float u = gl_TessCoord.x;
    float v = gl_TessCoord.y;

    // Bilinear interpolation of the four patch control points.  Vertices are
    // laid out as 0:(x0,z0) 1:(x1,z0) 2:(x1,z1) 3:(x0,z1).
    vec4 p1 = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, u);
    vec4 p2 = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, u);
    vec4 pos = mix(p1, p2, v);
    gl_Position = ubo.viewProj * pos;

    vec3 n1 = mix(inNormal[0], inNormal[1], u);
    vec3 n2 = mix(inNormal[3], inNormal[2], u);
    outNormal = normalize(mix(n1, n2, v));

    vec3 c1 = mix(inColor[0], inColor[1], u);
    vec3 c2 = mix(inColor[3], inColor[2], u);
    outColor = mix(c1, c2, v);

    vec2 uv1 = mix(inUV[0], inUV[1], u);
    vec2 uv2 = mix(inUV[3], inUV[2], u);
    outUV = mix(uv1, uv2, v);

    outWorldPos = pos.xyz;
    outViewVec = ubo.eye.xyz - pos.xyz;
}
