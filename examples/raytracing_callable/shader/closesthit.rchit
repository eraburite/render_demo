#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) rayPayloadInEXT vec3 hitValue;
// Outgoing callable payload: the callable shader writes it, this stage reads it
// back after executeCallableEXT() returns.
layout(location = 0) callableDataEXT vec3 outColor;

void main()
{
    // The three quads are the *same* BLAS instance-transformed to different x
    // positions, so gl_InstanceCustomIndexEXT identifies the quad.  Passing it
    // as the SBT record index selects callable shader 0 / 1 / 2 -- this is the
    // only mechanism that reuses one closest-hit shader for three materials.
    executeCallableEXT(gl_InstanceCustomIndexEXT, 0);
    hitValue = outColor;
}
