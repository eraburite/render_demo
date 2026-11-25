#version 450

layout (set = 0, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    vec4 eye;
    vec4 tessParams;
} ubo;

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inWorldPos;
layout (location = 4) in vec3 inViewVec;

layout (location = 0) out vec4 outFragColor;

void main()
{
    vec3 N = normalize(inNormal);
    vec3 L = normalize(vec3(0.5, 1.0, 0.3));
    vec3 V = normalize(inViewVec);
    vec3 R = reflect(-L, N);

    // Base gradient from the CPU-generated height colours; add snow on peaks.
    vec3 base = inColor;
    base = mix(base, vec3(0.92, 0.94, 0.98), smoothstep(8.0, 13.0, inWorldPos.y));

    float ambient = 0.25;
    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(R, V), 0.0), 24.0) * 0.2;
    vec3 color = (ambient + diffuse) * base + specular;

    // Fade distant terrain into the (dark) clear colour.
    float fog = clamp((length(inViewVec) - 45.0) / 110.0, 0.0, 0.75);
    color = mix(color, vec3(0.05, 0.06, 0.09), fog);

    outFragColor = vec4(color, 1.0);
}
