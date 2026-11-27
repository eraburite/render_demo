#version 450

// Draws the light quads themselves.  The four corners are already world-space
// positions in the light SSBO, so this needs neither a vertex buffer nor a model
// matrix: one instance per light, six vertices per quad.
struct AreaLight {
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 p3;
    vec4 color;
    vec4 params;
};
layout(std430, set = 2, binding = 0) readonly buffer AreaLights
{
    AreaLight lights[];
};

layout(std140, set = 0, binding = 0) uniform SceneBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 material;
    vec4 params;
} ubo;

layout(location = 0) out vec3 outColor;

void main()
{
    const AreaLight light = lights[gl_InstanceIndex];
    const vec3 corners[4] = vec3[4](light.p0.xyz, light.p1.xyz, light.p2.xyz, light.p3.xyz);
    const int order[6] = int[6](0, 1, 2, 0, 2, 3);
    const int index = order[gl_VertexIndex % 6];

    gl_Position = ubo.viewProj * vec4(corners[index], 1.0);
    // The emitters are shown at their emitted radiance, which is far above 1.0;
    // clamping here keeps the quad readable instead of a blown-out white blob.
    outColor = clamp(light.color.rgb * light.params.x * ubo.params.y * 0.06, 0.0, 1.0);
}
