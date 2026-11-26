#version 450

// Fullscreen triangle, same convention as examples/offscreen: no vertex
// buffer, gl_VertexIndex generates three vertices covering the viewport.
layout(location = 0) out vec2 fragUV;
void main()
{
    const vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
