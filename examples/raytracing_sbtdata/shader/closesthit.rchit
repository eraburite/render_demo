#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitValue;

// The point of this example: the colour is not in a uniform buffer, a push
// constant or a vertex attribute -- it lives in the SBT record that the instance
// selected.  gl_InstanceCustomIndexEXT is also available, but reading it here
// would defeat the demonstration, so the record is the only source.
layout(shaderRecordEXT, std430) buffer SbtRecord
{
    vec4 baseColor;
} record;

void main()
{
    hitValue = record.baseColor.rgb;
}
