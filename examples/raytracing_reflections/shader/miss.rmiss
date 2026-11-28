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

void main()
{
    // Dim vertical gradient instead of a flat colour: a mirror has to have
    // something worth reflecting, and the gradient makes a bounced ray visible in
    // the floor even where it misses every wall.
    const vec3 unitDirection = normalize(gl_WorldRayDirectionEXT);
    const float t = 0.5 * (unitDirection.y + 1.0);
    payload.color = mix(vec3(0.06, 0.08, 0.14), vec3(0.42, 0.50, 0.72), t);
    payload.distance = -1.0;
    payload.normal = vec3(0.0, 1.0, 0.0);
    payload.reflectivity = 0.0;
}
