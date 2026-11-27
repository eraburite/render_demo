#version 450

// Same instanced-glyph scheme as examples/text_rendering (DESIGN.md 26.3), but
// the rectangles are built in the *game's* coordinate space this time: pixels
// with +y down, matching the reference's TextRenderer, whose projection is
// `glm::ortho(0, width, height, 0)` and whose per-glyph top edge is
//     ypos = y + (H.bearing.y - glyph.Bearing.y) * scale.
// Because +y already points down, the atlas row order and the quad order agree
// and no v flip is needed (text_rendering had to flip because it used a y-up
// ortho).
layout(set = 0, binding = 0) uniform ScreenBlock
{
    mat4 projection; // ortho(0, width, height, 0), pixels, +y down
} ubo;

struct GlyphQuad
{
    vec4 rect;  // x0, y0 (top-left), x1, y1 in pixels, +y down
    vec4 uv;    // u0, v0, u1, v1; v0 is the top of the glyph in the atlas
    vec4 color; // rgb = text colour
};

layout(std430, set = 2, binding = 0) readonly buffer GlyphQuads
{
    GlyphQuad quads[];
};

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out vec3 fragColor;

const vec2 kCorners[6] = vec2[6](vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
                                 vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
    const GlyphQuad quad = quads[gl_InstanceIndex];
    const vec2 corner = kCorners[gl_VertexIndex % 6];

    gl_Position = ubo.projection * vec4(mix(quad.rect.xy, quad.rect.zw, corner), 0.0, 1.0);
    fragUV = mix(quad.uv.xy, quad.uv.zw, corner);
    fragColor = quad.color.rgb;
}
