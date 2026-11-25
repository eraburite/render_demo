#version 450

// Attributes matching VertexInputAtlas {ePosition, eColor, eUV, eNormal}.
layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inNormal;

// set0 binding0: camera viewProj + eye position (vertex stage).
layout (set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
    vec4 eye;
} ubo;

// set1 binding0: one world matrix per scene-graph mesh node.  The node is
// selected with gl_InstanceIndex, so a single draw renders the whole tree.
// model[3][3] is used as a per-node visibility flag: 1 = visible, 0 = hidden.
layout (set = 1, binding = 0) readonly buffer ModelMatrices {
    mat4 models[];
} instance;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec3 outViewVec;
layout (location = 3) out vec3 outLightVec;
layout (location = 4) flat out int outInstance;

void main()
{
    mat4 model = instance.models[gl_InstanceIndex];
    vec4 world = model * vec4(inPos, 1.0);
    gl_Position = ubo.viewProj * world;

    // Hidden nodes collapse to a point fully outside the clip volume; the
    // rasterizer discards them without any pipeline/buffer rebuild.
    if (model[3][3] < 0.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }

    outNormal = mat3(model) * inNormal;
    outColor = inColor;
    vec3 lightPos = vec3(2.5, 3.5, 4.0);
    outLightVec = lightPos - world.xyz;
    outViewVec = ubo.eye.xyz - world.xyz;
    outInstance = gl_InstanceIndex;
}
