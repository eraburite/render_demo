#version 450

// Geometry pass: world -> VIEW space.  The reference stores view-space position
// and normal in the G-buffer because the SSAO kernel is built in view space and
// the lighting pass treats the camera as the origin.  A UBO carries view+proj,
// an SSBO carries the per-instance model matrices (same atlas style as gbuffer).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;

layout(std140, set = 1, binding = 0) uniform CameraBlock
{
    mat4 view;
    mat4 proj;
} camera;

layout(std430, set = 2, binding = 0) readonly buffer Colors
{
    vec4 colors[];
} palette;
layout(std430, set = 3, binding = 0) readonly buffer Instances
{
    mat4 models[];
} instances;

layout(location = 0) out vec3 outViewPos;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec3 outAlbedo;

void main()
{
    const mat4 model = instances.models[gl_InstanceIndex];
    const vec4 world = model * vec4(inPos, 1.0);
    const vec4 viewPos = camera.view * world;
    gl_Position = camera.proj * viewPos;

    // view * inverse-transpose(model) == normal matrix into view space.
    vec3 worldNormal = transpose(inverse(mat3(model))) * inNormal;
    // Instance 0 is the room, seen from inside.  The reference flips its normals
    // (invertedNormals=1) so the walls are lit from within; do the same here.
    if (gl_InstanceIndex == 0) {
        worldNormal = -worldNormal;
    }
    outViewPos = viewPos.xyz;
    outViewNormal = mat3(camera.view) * worldNormal;
    outAlbedo = palette.colors[gl_InstanceIndex].rgb * inColor;
}
