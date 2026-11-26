#version 450

// GPU-displaced terrain, stage 3 of 4: tessellation evaluation.
//
// Two things happen per generated vertex:
//   * the heightmap is sampled at the interpolated UV and the flat patch is
//     pushed along its own normal by that height;
//   * a shading normal is rebuilt from the heightmap gradient, so the lighting
//     follows the terrain rather than the tessellation density.
//
// textureLod(..., 0.0) is deliberate: an implicit-LOD read inside a tessellation
// evaluation shader selects a mip from the derivatives of the *tessellated
// domain*, which change with the tessellation level -- the geometry would then
// move when the LOD changes.  The heightmap has exactly one mip level anyway,
// which is also why the CPU path's bilinear sample and this one agree.

layout (set = 0, binding = 0) uniform sampler2D heightMap;

layout (set = 1, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    mat4 view;
    vec4 eye;     // xyz: camera position in world space
    vec4 height;  // x: height span, y: height shift, z: grayscale flag
    vec4 terrain; // x: world width, y: world depth, z: one texel in u, w: one texel in v
    vec4 tess;    // x: min level, y: max level, z: min distance, w: max distance
} ubo;

layout (quads, fractional_odd_spacing, ccw) in;

layout (location = 0) in vec3 inPos[];
layout (location = 1) in vec2 inUV[];

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outWorldPos;
layout (location = 2) out float outHeight;

float sampleHeight(vec2 uv)
{
    return textureLod(heightMap, uv, 0.0).r * ubo.height.x - ubo.height.y;
}

vec3 sampleNormal(vec2 uv)
{
    vec2 stepU = vec2(ubo.terrain.z, 0.0);
    vec2 stepV = vec2(0.0, ubo.terrain.w);
    float hL = sampleHeight(uv - stepU);
    float hR = sampleHeight(uv + stepU);
    float hD = sampleHeight(uv - stepV);
    float hU = sampleHeight(uv + stepV);
    // Central differences turned into a world-space gradient, then the usual
    // (-dh/dx, 1, -dh/dz) surface normal.
    float dhdx = (hR - hL) / (2.0 * ubo.terrain.z * ubo.terrain.x);
    float dhdz = (hU - hD) / (2.0 * ubo.terrain.w * ubo.terrain.y);
    return normalize(vec3(-dhdx, 1.0, -dhdz));
}

void main()
{
    float u = gl_TessCoord.x;
    float v = gl_TessCoord.y;

    // Control point order 0,1,2,3 runs around the perimeter: u along 0->1 and
    // v along 0->3, so the two edges meeting at corner 0 are the interpolation
    // axes.
    vec3 p0 = gl_in[0].gl_Position.xyz;
    vec3 p1 = gl_in[1].gl_Position.xyz;
    vec3 p2 = gl_in[2].gl_Position.xyz;
    vec3 p3 = gl_in[3].gl_Position.xyz;

    vec3 base = mix(mix(p0, p1, u), mix(p3, p2, u), v);

    vec2 t0 = mix(inUV[0], inUV[1], u);
    vec2 t1 = mix(inUV[3], inUV[2], u);
    vec2 uv = mix(t0, t1, v);

    // The control points all sit in the y = 0 plane, so this is (0,1,0); taking
    // the cross product instead of hard-coding it keeps the displacement correct
    // if the patch grid is ever generated on a sloped base.
    vec3 patchNormal = normalize(cross(p3 - p0, p1 - p0));

    float h = sampleHeight(uv);
    vec3 world = base + patchNormal * h;

    outWorldPos = world;
    outHeight = world.y;
    outNormal = sampleNormal(uv);

    gl_Position = ubo.viewProj * vec4(world, 1.0);
}
