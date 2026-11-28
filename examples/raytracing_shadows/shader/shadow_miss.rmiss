#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 1) rayPayloadInEXT float shadowVisibility;

void main()
{
    // The shadow ray left the scene without hitting anything, so the light is
    // visible from the shading point.
    shadowVisibility = 1.0;
}
