#version 450
// Geometry pass: transform vertices into world space and forward the three
// G-Buffer channels (world position, world normal, albedo) to the MRT outputs.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(set = 1, binding = 0) uniform CameraMatrices { mat4 view; mat4 proj; } camera;
layout(set = 3, binding = 0) readonly buffer ModelMatrices { mat4 models[]; } instance;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec3 fragAlbedo;

void main()
{
    mat4 model = instance.models[gl_InstanceIndex];
    vec4 world = model * vec4(inPosition, 1.0);
    gl_Position = camera.proj * camera.view * world;
    fragWorldPos = world.xyz;
    // Inverse-transpose keeps normals correct under non-uniform scaling.
    fragWorldNormal = mat3(transpose(inverse(model))) * inNormal;
    fragAlbedo = inColor;
}
