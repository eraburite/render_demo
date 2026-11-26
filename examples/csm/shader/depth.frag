#version 450

// Depth-only fragment shader.  Empty on purpose: depth writes are fixed-function
// state, so the fragment shader has nothing to contribute -- but the stage is
// still declared (as in the reference) so no driver has to guess whether the
// pipeline has a fragment output interface.
void main()
{
}
