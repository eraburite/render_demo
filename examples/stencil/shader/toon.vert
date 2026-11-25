#version 450
// Pass 1: shaded object.  The pipeline's stencil op (ALWAYS -> REPLACE) marks
// every visible fragment with the dynamic reference value, producing the mask
// that pass 2 subtracts from its enlarged shell.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;

layout(binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
    mat4 model;
    vec4 lightDir;
    vec4 params; // x = outlineWidth (used by outline.vert only)
} ubo;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;

void main()
{
    mat4 modelView = ubo.view * ubo.model;
    gl_Position = ubo.proj * modelView * vec4(inPos, 1.0);
    outNormal = normalize(mat3(modelView) * inNormal);
    outColor = inColor;
}
