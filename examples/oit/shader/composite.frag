#version 450

// Screen pass: composite the transparent layers over the opaque image and write
// the result to the swapchain.  The reference does this in two steps (composite
// into the opaque framebuffer with SRC_ALPHA blending, then blit that to the
// backbuffer); both are folded here because the opaque image is an attachment of
// the previous pass and can only be *sampled*, not written, at this point.
//
// Equivalence, with reveal = product(1 - alpha_i) and avg = sum(w_i*c_i)/sum(w_i):
//
//   reference:  dst = avg * (1 - reveal) + opaque * reveal
//   here:       mix(opaque, avg, 1 - reveal)  ==  opaque*reveal + avg*(1-reveal)
//
// Identical.  See DESIGN.md 20.3.

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outFrag;

layout(set = 0, binding = 0) uniform sampler2D opaqueMap;
layout(set = 0, binding = 1) uniform sampler2D accumMap;
layout(set = 0, binding = 2) uniform sampler2D revealMap;

const float EPSILON = 0.00001;

bool approximatelyEqual(float a, float b)
{
    return abs(a - b) <= max(abs(a), abs(b)) * EPSILON;
}

void main()
{
    const vec3 opaque = texture(opaqueMap, inUV).rgb;

    // reveal == 1 means no transparent fragment ever touched this texel.  Skip
    // the accum fetch and the division instead of dividing by ~0.
    const float reveal = texture(revealMap, inUV).r;
    if (approximatelyEqual(reveal, 1.0)) {
        outFrag = vec4(opaque, 1.0);
        return;
    }

    const vec4 accum = texture(accumMap, inUV);
    // The accumulation target is 16-bit float; a few thousand overlapping
    // layers can overflow it to infinity.  Clamping to the accumulated weight
    // keeps the average finite (the reference does the same).
    vec3 accumColor = accum.rgb;
    if (any(isinf(accumColor))) {
        accumColor = vec3(accum.a);
    }

    const vec3 average = accumColor / max(accum.a, EPSILON);
    outFrag = vec4(mix(opaque, average, 1.0 - reveal), 1.0);
}
