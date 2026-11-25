#version 450

// Pure normal-based lighting; the deer model has no textures.
layout (location = 0) in vec3 inNormal;

layout (location = 0) out vec4 outFragColor;

void main()
{
    vec3 N = normalize(inNormal);
    vec3 L = normalize(vec3(0.4, 0.8, 0.6));
    float diffuse = max(dot(N, L), 0.0);
    vec3 color = vec3(0.12, 0.14, 0.20) + diffuse * vec3(0.78, 0.80, 0.86);
    outFragColor = vec4(color, 1.0);
}
