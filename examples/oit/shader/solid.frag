#version 450

// Opaque pass: straight color into the RGBA16F opaque target, depth written by
// the fixed-function depth test (the pipeline enables depth write here).

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outFrag;

void main()
{
    // Alpha is forced to 1: the opaque target is sampled as plain RGB by the
    // screen pass, and a translucent alpha there would be meaningless.
    outFrag = vec4(inColor.rgb, 1.0);
}
