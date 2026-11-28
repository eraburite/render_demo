#version 460
#extension GL_EXT_ray_tracing : require

// Occlusion hit group: reaching this shader means the shadow ray hit something,
// which is already recorded by shadowVisibility staying at 0.0.  Doing nothing
// (and not touching the payload) is the whole point -- shading a blocker is
// wasted work for a visibility query.
void main() {}
