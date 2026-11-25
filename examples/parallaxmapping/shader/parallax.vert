#version 450

// Parallax mapping vertex stage (reference: data/shaders/glsl/parallaxmapping/
// parallax.vert).  The reference transforms the light and eye positions into
// tangent space here, but this engine's descriptor tables declare the eye
// position for the fragment stage only (VUID-VkGraphicsPipelineCreateInfo-
// layout-07988 forbids reading it here), so this stage forwards the world-space
// TBN basis and the fragment stage does the transpose instead.  The result is
// the same tangent-space frame the reference builds.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;

layout(set = 1, binding = 0) uniform CameraBlock {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform PushBlock {
    mat4 model;
    vec4 lightPos;      // world space; animates along x/z
    float heightScale;
    float parallaxBias;
    float numLayers;
    int mappingMode;
} push;

layout(location = 0) out vec2 outUV;
// Columns are the world-space tangent, bitangent and normal.
layout(location = 1) out mat3 outTBN;
layout(location = 4) out vec3 outWorldPos;

void main()
{
    const vec3 worldPos = vec3(push.model * vec4(inPosition, 1.0));

    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);

    outUV = inTexCoord;
    outWorldPos = worldPos;

    // The plane demo only ever uses a uniform model scale, so the model matrix
    // cannot skew the basis and a plain mat3 rotation is enough.
    const vec3 N = normalize(mat3(push.model) * inNormal);
    const vec3 T = normalize(mat3(push.model) * inTangent);
    const vec3 B = normalize(cross(N, T));
    outTBN = mat3(T, B, N);
}
