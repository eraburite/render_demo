#version 450
// Pass 2: extrude the same mesh along its normals.  Depth testing is off and
// the stencil test is NOT_EQUAL against the value pass 1 wrote, so only the
// ring of texels outside the original silhouette survives.  The UBO block must
// be declared identically to toon.vert (std140 layout is per-block), even
// though only params.x is used here.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;

layout(binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
    mat4 model;
    vec4 lightDir;
    vec4 params; // x = outlineWidth
} ubo;

void main()
{
    vec3 displaced = inPos + normalize(inNormal) * ubo.params.x;
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(displaced, 1.0);
}
