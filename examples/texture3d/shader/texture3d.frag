#version 450

// 3D texture volume rendering, ray-marched instead of slice-stacked.
//
// The reference (texture3d.frag) uploads one quad per volume slice and lets
// fixed-function blending stack them, which needs one draw call per slice
// (128 of them).  This framework renders one operation per draw, so a single
// cube is drawn and the volume is integrated analytically along the view ray
// inside the fragment shader: one draw call, no per-slice geometry or uniform
// rewrites.  The samples are composited front to back with the same alpha
// blending the reference gets from its slice quads.

layout(set = 0, binding = 0) uniform sampler3D volumeTex;

layout(set = 2, binding = 0) uniform SceneBlock {
    vec4 eyePosition;
} scene;

layout(push_constant) uniform PushBlock {
    mat4 model;
    float densityGain;
    float scroll;
    float time;
    int steps;
} push;

layout(location = 0) in vec3 inObjectPos;

layout(location = 0) out vec4 outColor;

const int MAX_STEPS = 128;
const float DENSITY_THRESHOLD = 0.42;

void main()
{
    // Object space is the unit cube [-0.5, 0.5]^3, so the texture coordinate is
    // simply the position shifted by half a unit.
    // The push block is capped at the 128-byte guaranteed minimum, so the
    // world->object matrix is derived here (once per fragment, on a 36-vertex
    // cube) instead of being uploaded per draw.
    const mat4 inverseModel = inverse(push.model);
    const vec3 eyeObject = (inverseModel * vec4(scene.eyePosition.xyz, 1.0)).xyz;

    // The rasterised fragment is the cube's near face; the ray continues from
    // there along the eye->fragment direction, i.e. into the volume.
    const vec3 rayDir = normalize(inObjectPos - eyeObject);
    const float stepSize = 1.7 / float(push.steps);

    vec3 pos = inObjectPos + rayDir * 0.002;
    vec3 accumColor = vec3(0.0);
    float accumAlpha = 0.0;

    for (int i = 0; i < MAX_STEPS; ++i) {
        if (i >= push.steps || accumAlpha > 0.985) {
            break;
        }
        pos += rayDir * stepSize;
        if (any(lessThan(pos, vec3(-0.5))) || any(greaterThan(pos, vec3(0.5)))) {
            break;
        }

        const vec3 uvw = pos + vec3(0.5) + vec3(0.0, push.scroll, 0.0);
        const float density = textureLod(volumeTex, uvw, 0.0).r;

        // Cheap lateral offset towards a fixed light: brighter wisps facing the
        // light, darker ones behind them, which reads as self-shadowing.
        const float shade = textureLod(volumeTex, uvw + vec3(0.0, 0.08, 0.0), 0.0).r;
        const vec3 sampleColor = mix(vec3(0.18, 0.38, 0.85), vec3(1.0, 0.86, 0.55), shade);

        const float alpha = clamp((density - DENSITY_THRESHOLD) * push.densityGain, 0.0, 1.0)
                            * stepSize * 14.0;
        accumColor += (1.0 - accumAlpha) * alpha * sampleColor;
        accumAlpha += (1.0 - accumAlpha) * alpha;
    }

    // The pipeline blends with src-alpha, so hand it the un-premultiplied colour.
    outColor = vec4(accumColor / max(accumAlpha, 1e-3), accumAlpha);
}
