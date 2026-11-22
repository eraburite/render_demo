#version 450
struct Particle { vec4 position; vec4 velocity; vec4 color; };
layout(std430, set = 0, binding = 2) readonly buffer Particles { Particle particles[]; };
// view + proj (128 bytes).  The view rows give the camera basis the billboards
// need; with only view * proj the quad corner could not be oriented towards the
// camera, which is why the old version offset it along world X/Y and the quads
// turned edge-on as soon as the camera looked from another angle.
layout(push_constant) uniform ViewParams { mat4 view; mat4 proj; } viewParams;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

const vec2 corners[6] = vec2[6](
    vec2(-1,-1), vec2(1,-1), vec2(1,1), vec2(-1,-1), vec2(1,1), vec2(-1,1));

const float PARTICLE_HALF_SIZE = 0.015;

void main() {
    uint particleIndex = gl_VertexIndex / 6;
    uint cornerIndex = gl_VertexIndex % 6;
    Particle p = particles[particleIndex];
    vec2 corner = corners[cornerIndex];
    // Rows 0 and 1 of the view matrix are the camera right / up vectors in world
    // space (glm is column-major, so the row components live at [col][row]).
    vec3 cameraRight = vec3(viewParams.view[0][0], viewParams.view[1][0], viewParams.view[2][0]);
    vec3 cameraUp = vec3(viewParams.view[0][1], viewParams.view[1][1], viewParams.view[2][1]);
    vec3 world = p.position.xyz
                 + (cameraRight * corner.x + cameraUp * corner.y) * PARTICLE_HALF_SIZE;
    gl_Position = viewParams.proj * viewParams.view * vec4(world, 1.0);
    fragColor = p.color;
    fragUV = corner;
}
