#version 450
// Fullscreen triangle generated from gl_VertexIndex; no vertex buffer needed.
// UV runs 0..2 across the triangle and interpolates to 0..1 in the viewport.
layout(location = 0) out vec2 fragTexCoord;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragTexCoord = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
