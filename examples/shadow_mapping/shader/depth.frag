#version 450

// Empty on purpose: depth writes are fixed-function state.  The stage is still
// declared so the pipeline has an explicit fragment output interface, matching
// the reference's depth fragment shader.
void main()
{
}
