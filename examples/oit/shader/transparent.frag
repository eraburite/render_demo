#version 450

// Transparent pass: weighted-blended OIT.  Two color targets with *different*
// blend state (see GraphicsPipelineConfig::blendAttachments):
//
//   attachment 0 (accum)  dst = src + dst        one/one
//   attachment 1 (reveal) dst = dst * (1 - src)  zero/one-minus-src-color
//
// Both operations are commutative, which is the whole point: the draw order of
// transparent geometry does not affect the result.

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outAccum;
layout(location = 1) out float outReveal;

void main()
{
    // Depth-weighted weight function from the reference implementation: nearer
    // fragments contribute more, and low-alpha fragments are boosted so that a
    // barely visible surface still influences the average.
    const float weight = clamp(
        pow(min(1.0, inColor.a * 10.0) + 0.01, 3.0) * 1e8 * pow(1.0 - gl_FragCoord.z * 0.9, 3.0),
        1e-2, 3e3);

    // Premultiplied color times weight.  The alpha channel accumulates the
    // weights themselves, which is what makes the division in the screen pass
    // a weighted average.
    outAccum = vec4(inColor.rgb * inColor.a, inColor.a) * weight;

    // "Revealage": the product of (1 - alpha) of every transparent layer.  The
    // multiplicative blend above leaves exactly that product in the target.
    outReveal = inColor.a;
}
