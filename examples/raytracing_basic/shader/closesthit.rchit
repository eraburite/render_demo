#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitValue;

// Barycentric coordinates of the hit point, supplied by the fixed-function
// intersection stage.  Using them as colours makes the two triangles of the
// quad distinguishable and proves both BLAS primitives are reachable.
hitAttributeEXT vec2 attribs;

void main()
{
    const vec3 barycentric = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    const vec3 tint0 = vec3(0.95, 0.35, 0.25);
    const vec3 tint1 = vec3(0.25, 0.85, 0.45);
    const vec3 tint2 = vec3(0.30, 0.45, 0.95);
    hitValue = tint0 * barycentric.x + tint1 * barycentric.y + tint2 * barycentric.z;
}
