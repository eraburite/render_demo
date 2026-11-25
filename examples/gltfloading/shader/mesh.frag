#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inViewPos;
layout(location = 3) flat in int inNode;

layout(location = 0) out vec4 outFragColor;

void main()
{
    vec3 N = normalize(inNormal);
    // Light attached to the camera, expressed in view space (camera looks down -Z).
    vec3 L = normalize(vec3(0.45, 0.8, 0.9));
    vec3 V = normalize(-inViewPos);
    vec3 H = normalize(L + V);

    // A different tint per node makes the node hierarchy easy to read.  deer.gltf
    // has no vertex colors, so this is the only source of albedo variation.
    vec3 tints[4] = vec3[4](
        vec3(0.90, 0.72, 0.52),
        vec3(0.55, 0.75, 0.90),
        vec3(0.75, 0.62, 0.88),
        vec3(0.90, 0.85, 0.60));
    vec3 albedo = inColor * tints[inNode % 4];

    vec3 ambient = 0.16 * albedo;
    vec3 diffuse = max(dot(N, L), 0.0) * albedo;
    vec3 specular = pow(max(dot(N, H), 0.0), 24.0) * vec3(0.25);
    outFragColor = vec4(ambient + diffuse + specular, 1.0);
}
