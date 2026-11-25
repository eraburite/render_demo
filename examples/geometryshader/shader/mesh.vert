#version 450

// Attributes matching VertexInputAtlas {ePosition, eColor, eUV, eNormal}.
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inNormal;

layout (set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
} ubo;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec3 outViewVec;
layout (location = 3) out vec3 outLightVec;

void main()
{
    vec4 worldPos = ubo.viewProj * vec4(inPos, 1.0);
    gl_Position = worldPos;
    // The model is pre-transformed into world space during loading, so the
    // normal needs no extra matrix here.  Lighting is done in view space
    // using the camera at origin looking down -Z (view matrix folded into
    // viewProj).
    outNormal = inNormal;
    outColor = inColor;
    vec3 lightPos = vec3(1.0, 1.0, 1.0);
    outLightVec = lightPos - inPos;
    outViewVec = -inPos;
}
