#version 450

// PN-triangles evaluation shader: evaluates the cubic Bezier patch built by
// pntriangles.tesc and blends it with the barycentric (flat) approximation.

struct PnPatch {
    float b210;
    float b120;
    float b021;
    float b012;
    float b102;
    float b201;
    float b111;
    float n110;
    float n011;
    float n101;
};

layout (set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
} ubo;

// 0 = left half of the screen, 1 = right half.
layout (constant_id = 0) const uint uSide = 1u;
// 0 = flat barycentric, 1 = full PN surface.
layout (constant_id = 2) const float uTessAlpha = 1.0;

layout (triangles, fractional_odd_spacing, cw) in;

layout (location = 0) in vec3 iNormal[];
layout (location = 3) in PnPatch iPnPatch[];

layout (location = 0) out vec3 outNormal;

#define uvw gl_TessCoord

void main()
{
    vec3 uvwSquared = uvw * uvw;
    vec3 uvwCubed = uvwSquared * uvw;

    // Gather a control value per component from the three patch vertices.
    vec3 b210 = vec3(iPnPatch[0].b210, iPnPatch[1].b210, iPnPatch[2].b210);
    vec3 b120 = vec3(iPnPatch[0].b120, iPnPatch[1].b120, iPnPatch[2].b120);
    vec3 b021 = vec3(iPnPatch[0].b021, iPnPatch[1].b021, iPnPatch[2].b021);
    vec3 b012 = vec3(iPnPatch[0].b012, iPnPatch[1].b012, iPnPatch[2].b012);
    vec3 b102 = vec3(iPnPatch[0].b102, iPnPatch[1].b102, iPnPatch[2].b102);
    vec3 b201 = vec3(iPnPatch[0].b201, iPnPatch[1].b201, iPnPatch[2].b201);
    vec3 b111 = vec3(iPnPatch[0].b111, iPnPatch[1].b111, iPnPatch[2].b111);

    vec3 n110 = normalize(vec3(iPnPatch[0].n110, iPnPatch[1].n110, iPnPatch[2].n110));
    vec3 n011 = normalize(vec3(iPnPatch[0].n011, iPnPatch[1].n011, iPnPatch[2].n011));
    vec3 n101 = normalize(vec3(iPnPatch[0].n101, iPnPatch[1].n101, iPnPatch[2].n101));

    // Same (z, x, y) -> (P0, P1, P2) mapping as the pass-through shader.
    vec3 barNormal = uvw.z * iNormal[0] + uvw.x * iNormal[1] + uvw.y * iNormal[2];
    vec3 pnNormal = iNormal[0] * uvwSquared[2] + iNormal[1] * uvwSquared[0] + iNormal[2] * uvwSquared[1]
                  + n110 * uvw[2] * uvw[0] + n011 * uvw[0] * uvw[1] + n101 * uvw[2] * uvw[1];
    outNormal = normalize(uTessAlpha * pnNormal + (1.0 - uTessAlpha) * barNormal);

    vec3 barPos = uvw.z * gl_in[0].gl_Position.xyz
                + uvw.x * gl_in[1].gl_Position.xyz
                + uvw.y * gl_in[2].gl_Position.xyz;

    uvwSquared *= 3.0;
    vec3 pnPos = gl_in[0].gl_Position.xyz * uvwCubed[2]
               + gl_in[1].gl_Position.xyz * uvwCubed[0]
               + gl_in[2].gl_Position.xyz * uvwCubed[1]
               + b210 * uvwSquared[2] * uvw[0]
               + b120 * uvwSquared[0] * uvw[2]
               + b201 * uvwSquared[2] * uvw[1]
               + b021 * uvwSquared[0] * uvw[1]
               + b102 * uvwSquared[1] * uvw[2]
               + b012 * uvwSquared[1] * uvw[0]
               + b111 * 6.0 * uvw[0] * uvw[1] * uvw[2];

    vec3 finalPos = (1.0 - uTessAlpha) * barPos + uTessAlpha * pnPos;

    vec4 clip = ubo.viewProj * vec4(finalPos, 1.0);
    // NDC x in [-1,1] -> [-1,0] (left) or [0,1] (right).
    float side = float(uSide) * 2.0 - 1.0;
    clip.x = 0.5 * clip.x + 0.5 * side * clip.w;
    gl_Position = clip;
}
