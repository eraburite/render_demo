#version 460
#extension GL_EXT_ray_tracing : require

struct RayPayload
{
    vec3 color;
    float distance;
    vec3 normal;
    float reflectivity;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

hitAttributeEXT vec2 attribs;

layout(binding = 3, set = 0) readonly buffer Materials
{
    // albedo.rgb + reflectivity in params.x (see test46.cpp).
    vec4 materialData[];
} materials;

// Geometry is fetched through the same buffers the acceleration structure was
// built from: a closest-hit shader has no vertex fetch stage of its own, so the
// primitive it just hit has to be reconstructed from gl_PrimitiveID.
layout(binding = 4, set = 0) readonly buffer Positions
{
    vec4 position[];
} positions;

layout(binding = 5, set = 0) readonly buffer Normals
{
    vec4 normal[];
} normals;

layout(binding = 6, set = 0) readonly buffer Indices
{
    uint index[];
} indices;

void main()
{
    const uint base = 3u * uint(gl_PrimitiveID);
    const uint i0 = indices.index[base + 0u];
    const uint i1 = indices.index[base + 1u];
    const uint i2 = indices.index[base + 2u];

    // Object -> world uses the per-instance transform the TLAS carries; the
    // examples give every instance a non-uniform scale, so the normal is taken
    // from the transformed edges rather than from a cached buffer value.
    const vec3 p0 = gl_ObjectToWorldEXT * vec4(positions.position[i0].xyz, 1.0);
    const vec3 p1 = gl_ObjectToWorldEXT * vec4(positions.position[i1].xyz, 1.0);
    const vec3 p2 = gl_ObjectToWorldEXT * vec4(positions.position[i2].xyz, 1.0);

    const vec3 barycentric = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    const vec3 objectNormal = normals.normal[i0].xyz * barycentric.x
                            + normals.normal[i1].xyz * barycentric.y
                            + normals.normal[i2].xyz * barycentric.z;
    // Cross of the world-space edges flips along with a mirrored transform and
    // stays unit length under uniform scale; both matter for reflect().
    vec3 worldNormal = normalize(cross(p1 - p0, p2 - p0));
    if (dot(worldNormal, objectNormal) < 0.0) {
        worldNormal = -worldNormal;
    }

    const vec4 albedo = materials.materialData[2u * uint(gl_InstanceCustomIndexEXT) + 0u];
    const vec4 params = materials.materialData[2u * uint(gl_InstanceCustomIndexEXT) + 1u];

    const vec3 lightDirection = normalize(vec3(0.35, 1.0, 0.45));
    const float nDotL = max(dot(worldNormal, lightDirection), 0.0f);

    payload.color = albedo.rgb * (0.18 + 0.82 * nDotL);
    payload.distance = gl_HitTEXT;
    payload.normal = worldNormal;
    payload.reflectivity = params.x;
}
