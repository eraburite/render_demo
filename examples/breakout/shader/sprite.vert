#version 450

// One instance per sprite quad.  The reference (Breakout's SpriteRenderer)
// uploads a `model` matrix + `spriteColor` uniform and issues one draw per
// sprite -- ~50 draws per frame for bricks, paddle, ball and power-ups.  Here
// the per-sprite state is an instance record and the six corners come from
// gl_VertexIndex, so every sprite that shares a texture becomes one instanced
// draw (see DESIGN.md 27.3).
//
// Coordinates are pixels with +y pointing down, exactly like the reference's
// `glm::ortho(0, width, height, 0)`: corner (0,0) is the sprite's top-left,
// which is also the top-left of the texture (row 0 of the file), so no uv flip
// is needed anywhere.
layout(set = 0, binding = 0) uniform CameraBlock
{
    mat4 projection;
} camera;

struct SpriteInstance
{
    vec2 position; // top-left corner, pixels
    vec2 size;     // width, height, pixels
    vec4 color;    // rgb tint (the reference's spriteColor); a unused
};

layout(std430, set = 1, binding = 0) readonly buffer SpriteInstances
{
    SpriteInstance instances[];
};

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

// The same six corners the reference emits (top-left, bottom-left,
// bottom-right; top-left, bottom-right, top-right).
const vec2 kCorners[6] = vec2[6](vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
                                 vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
    const SpriteInstance sprite = instances[gl_InstanceIndex];
    const vec2 corner = kCorners[gl_VertexIndex % 6];

    fragUV = corner;
    fragColor = sprite.color;
    gl_Position = camera.projection * vec4(sprite.position + corner * sprite.size, 0.0, 1.0);
}
