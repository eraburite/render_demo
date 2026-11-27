#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 lightDir = normalize(vec3(0.4, 0.8, 0.6));
    const float diffuse = 0.35 + 0.65 * max(dot(normalize(fragNormal), lightDir), 0.0);
    outColor = vec4(fragColor * diffuse, 1.0);
}
