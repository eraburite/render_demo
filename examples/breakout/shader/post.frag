#version 450

// Reference: three 3x3 uniforms (offsets, edge_kernel, blur_kernel) plus the
// three flags.  The two kernels are compile-time constants in the reference too
// -- they are uploaded once and never change -- and the sample offsets are a
// fixed 1/300 of the screen.  Baking them here removes 72 bytes of uniform
// traffic and, more usefully, makes the shader readable: what is left in the
// UBO is only what actually varies per frame.
layout(set = 0, binding = 0) uniform PostBlock
{
    // x = time, y = confuse, z = chaos, w = shake
    vec4 params;
} post;

layout(set = 1, binding = 0) uniform sampler2D sceneTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

const float kOffset = 1.0 / 300.0;
const vec2 kOffsets[9] = vec2[9](vec2(-kOffset, kOffset), vec2(0.0, kOffset), vec2(kOffset, kOffset),
                                 vec2(-kOffset, 0.0), vec2(0.0, 0.0), vec2(kOffset, 0.0),
                                 vec2(-kOffset, -kOffset), vec2(0.0, -kOffset), vec2(kOffset, -kOffset));

// The reference's edge_kernel: -1 all around, 8 in the centre.
const float kEdgeKernel[9] = float[9](-1.0, -1.0, -1.0, -1.0, 8.0, -1.0, -1.0, -1.0, -1.0);
// and blur_kernel: a 3x3 binomial, already normalised.
const float kBlurKernel[9] = float[9](1.0 / 16.0, 2.0 / 16.0, 1.0 / 16.0,
                                      2.0 / 16.0, 4.0 / 16.0, 2.0 / 16.0,
                                      1.0 / 16.0, 2.0 / 16.0, 1.0 / 16.0);

void main()
{
    vec3 color = vec3(0.0);

    if (post.params.z > 0.5) // chaos: edge detection
    {
        for (int i = 0; i < 9; ++i)
        {
            color += texture(sceneTexture, fragUV + kOffsets[i]).rgb * kEdgeKernel[i];
        }
    }
    else if (post.params.y > 0.5) // confuse: colour inversion
    {
        color = 1.0 - texture(sceneTexture, fragUV).rgb;
    }
    else if (post.params.w > 0.5) // shake: blur
    {
        for (int i = 0; i < 9; ++i)
        {
            color += texture(sceneTexture, fragUV + kOffsets[i]).rgb * kBlurKernel[i];
        }
    }
    else
    {
        color = texture(sceneTexture, fragUV).rgb;
    }

    // The reference zero-initialises `color` and only assigns .a in the two
    // convolution branches; the scene target is opaque, so a constant 1 is what
    // it effectively had in every branch.
    outColor = vec4(color, 1.0);
}
