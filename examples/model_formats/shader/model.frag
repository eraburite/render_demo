#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 normal = length(fragNormal) > 0.001 ? normalize(fragNormal) : vec3(0.0, 0.0, 1.0);
    const float diffuse = max(dot(normal, normalize(vec3(0.4, 0.9, 0.6))), 0.0);
    // A faint checker keeps the UV path observable without needing a texture.
    const float checker = 0.92 + 0.08 * float((int(fragUV.x * 8.0) + int(fragUV.y * 8.0)) % 2 == 0);
    outColor = vec4(fragColor * (0.30 + 0.70 * diffuse) * checker, 1.0);
}
