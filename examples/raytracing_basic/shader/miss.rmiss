#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitValue;

void main()
{
    // Deep blue background.  The exact value is mirrored by --verify so the hit
    // ratio can be measured without re-deriving it from the dump.
    hitValue = vec3(0.05, 0.08, 0.20);
}
