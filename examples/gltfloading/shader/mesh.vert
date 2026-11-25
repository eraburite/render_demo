#version 450

// Attributes mirror the engine Vertex layout {ePosition, eColor, eUV, eNormal}.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;

// set1 binding0: the shared camera UBO (view + proj).
layout(set = 1, binding = 0) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
} camera;

// set3 binding0: one world matrix per mesh-bearing node.  The engine draws
// all of them as instances, so gl_InstanceIndex selects the node's matrix.
layout(set = 3, binding = 0) readonly buffer ModelMatrices {
    mat4 models[];
} instance;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragViewPos;
layout(location = 3) flat out int fragNode;

void main()
{
    mat4 M = instance.models[gl_InstanceIndex];
    vec4 world = M * vec4(inPosition, 1.0);
    vec4 viewPos = camera.view * world;
    gl_Position = camera.proj * viewPos;

    // Normal matrix of the node transform, then into view space so the
    // fragment shader can use a camera-relative light direction.
    fragNormal = mat3(camera.view) * mat3(transpose(inverse(M))) * inNormal;
    fragColor = inColor;
    fragViewPos = viewPos.xyz;
    fragNode = gl_InstanceIndex;
}
