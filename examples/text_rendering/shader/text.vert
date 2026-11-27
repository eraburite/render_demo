#version 450

// One instance per glyph quad.  The reference uploads six vertices per glyph
// into a VBO and draws them immediately; here the corner index comes from
// gl_VertexIndex (0..5) and the rectangle, atlas rect and colour come from a
// per-frame instance buffer.  That keeps a whole text run at one draw call and
// -- more importantly for this framework -- keeps every descriptor static
// (see DESIGN.md 26.3).
layout(set = 0, binding = 0) uniform ScreenBlock
{
    mat4 projection; // ortho(0, width, 0, height) with y already flipped for Vulkan
} ubo;

struct GlyphQuad
{
    vec4 rect;  // x0, y0, x1, y1 in pixels, +y up, origin bottom-left
    vec4 uv;    // u0, v0, u1, v1, v0 = top of the glyph in the atlas
    vec4 color; // rgb = text colour
};

layout(std430, set = 2, binding = 0) readonly buffer GlyphQuads
{
    GlyphQuad quads[];
};

layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out vec3 fragColor;

// Two triangles in the same order the reference emits them (top-left, bottom-left,
// bottom-right; top-left, bottom-right, top-right), expressed as a corner in
// [0,1]^2 so the same table works for the atlas rect.
const vec2 kCorners[6] = vec2[6](vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
                                 vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
    const GlyphQuad quad = quads[gl_InstanceIndex];
    const vec2 corner = kCorners[gl_VertexIndex % 6];

    const vec2 position = mix(quad.rect.xy, quad.rect.zw, corner);
    gl_Position = ubo.projection * vec4(position, 0.0, 1.0);

    // The quad's +y is up, but the atlas stores row 0 at the top, so the v axis
    // has to be flipped: top of the quad samples v0.
    const vec2 uvCorner = vec2(corner.x, 1.0 - corner.y);
    fragUV = mix(quad.uv.xy, quad.uv.zw, uvCorner);
    fragColor = quad.color.rgb;
}
