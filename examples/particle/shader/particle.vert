#version 450
struct Particle { vec4 position; vec4 velocity; vec4 color; };
layout(std430, set = 0, binding = 2) readonly buffer Particles { Particle particles[]; };
layout(push_constant) uniform ViewParams { mat4 viewProj; } viewParams;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

const vec2 corners[6] = vec2[6](
    vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));

void main() {
    uint particleIndex = gl_VertexIndex / 6;
    uint cornerIndex = gl_VertexIndex % 6;
    Particle p = particles[particleIndex];
    vec2 corner = corners[cornerIndex];
    vec3 world = p.position.xyz + vec3(corner * 0.02, 0.0);
    gl_Position = viewParams.viewProj * vec4(world, 1.0);
    fragColor = p.color;
    fragUV = corner;
}
