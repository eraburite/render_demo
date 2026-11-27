#version 450

// The reference rebinds `offset` and `color` uniforms and draws six vertices
// for every live particle -- up to 500 draw calls per frame.  Here the live
// particles are compacted into one instance buffer on the CPU and drawn with a
// single instanced draw; the dead ones are simply not in the buffer.
layout(set = 0, binding = 0) uniform CameraBlock
{
    mat4 projection;
} camera;

struct ParticleInstance
{
    vec2 position; // top-left corner, pixels
    vec4 color;    // rgba, alpha fades out over the particle's life
};

layout(std430, set = 1, binding = 0) readonly buffer ParticleInstances
{
    ParticleInstance particles[];
};

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

// `float scale = 10.0f;` in the reference's particle.vert.
const float kParticleScale = 10.0;

const vec2 kCorners[6] = vec2[6](vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0),
                                 vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main()
{
    const ParticleInstance particle = particles[gl_InstanceIndex];
    const vec2 corner = kCorners[gl_VertexIndex % 6];

    fragUV = corner;
    fragColor = particle.color;
    // Reference: projection * vec4((vertex.xy * scale) + offset, 0, 1), with the
    // quad built from the same corners.
    gl_Position = camera.projection * vec4(particle.position + corner * kParticleScale, 0.0, 1.0);
}
