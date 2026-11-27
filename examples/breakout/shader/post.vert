#version 450

// The reference draws a [-1,1] quad and does all its displacement work in the
// vertex shader: `chaos` shifts the sampled uv, `confuse` flips it, `shake`
// nudges gl_Position.  A full-screen triangle covers the same area with three
// vertices, and the uv is still exactly (0,0)..(1,1) over the visible region.
//
// Note the y direction: Vulkan's NDC has +y pointing down, so uv.y = 0 is the
// top row of the scene target, which is also the top of the window.  No flip is
// needed -- unlike the GL reference, whose framebuffer texture is bottom-up.
layout(set = 0, binding = 0) uniform PostBlock
{
    // x = time, y = confuse, z = chaos, w = shake
    vec4 params;
} post;

layout(location = 0) out vec2 fragUV;

void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexIndex];
    vec2 uv = position * 0.5 + 0.5;

    if (post.params.z > 0.5)
    {
        // Reference: strength = 0.3, uv += (sin(time), cos(time)) * strength.
        const float strength = 0.3;
        uv = vec2(uv.x + sin(post.params.x) * strength, uv.y + cos(post.params.x) * strength);
    }
    else if (post.params.y > 0.5)
    {
        uv = vec2(1.0 - uv.x, 1.0 - uv.y);
    }

    if (post.params.w > 0.5)
    {
        const float strength = 0.01;
        position.x += cos(post.params.x * 10.0) * strength;
        position.y += cos(post.params.x * 15.0) * strength;
    }

    fragUV = uv;
    gl_Position = vec4(position, 0.0, 1.0);
}
