#version 450

// 2.gamma_correction's two coupled changes:
//   * sRGB texture upload (hardware linearises samples) vs plain UNORM, and
//   * 1/d^2 attenuation plus 1/2.2 display encode vs 1/d with no encode.
// Both are switched by the same flag, so the two modes are directly comparable.
layout(set = 1, binding = 0) uniform sampler2D linearTexture;
layout(set = 1, binding = 1) uniform sampler2D srgbTexture;

layout(std140, set = 0, binding = 0) uniform GammaBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 params;
} ubo;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in vec4 inMaterial;
layout(location = 0) out vec4 outColor;

const vec3 LIGHT_POS[4] = vec3[4](
    vec3(-3.0, 1.0, 0.0),
    vec3(-1.0, 1.0, 0.0),
    vec3(1.0, 1.0, 0.0),
    vec3(3.0, 1.0, 0.0));
const vec3 LIGHT_COLOR[4] = vec3[4](
    vec3(0.25), vec3(0.50), vec3(0.75), vec3(1.00));

void main()
{
    const bool gamma = ubo.params.x > 0.5;
    if (inMaterial.a > 0.5) {
        // Markers are authored in display space; only the gamma path encodes
        // them again, mirroring the reference's final pow().
        outColor = vec4(gamma ? pow(inMaterial.rgb, vec3(1.0 / 2.2)) : inMaterial.rgb, 1.0);
        return;
    }

    const vec3 albedo = gamma ? texture(srgbTexture, inUV).rgb : texture(linearTexture, inUV).rgb;
    const vec3 normal = normalize(inNormal);
    const vec3 viewDir = normalize(ubo.eye.xyz - inWorldPos);
    vec3 lighting = vec3(0.0);

    for (int i = 0; i < 4; ++i) {
        const vec3 toLight = LIGHT_POS[i] - inWorldPos;
        const float distance = max(length(toLight), 0.01);
        const vec3 lightDir = toLight / distance;
        const float diffuse = max(dot(normal, lightDir), 0.0);
        const vec3 halfway = normalize(lightDir + viewDir);
        const float specular = pow(max(dot(normal, halfway), 0.0), 64.0);
        // The reference deliberately switches the *attenuation law* with the
        // gamma flag; 1/d is physically wrong but makes the difference obvious.
        const float attenuation = gamma ? 1.0 / (distance * distance) : 1.0 / distance;
        lighting += (diffuse + specular) * LIGHT_COLOR[i] * attenuation;
    }

    vec3 result = albedo * lighting;
    if (gamma) {
        result = pow(max(result, vec3(0.0)), vec3(1.0 / 2.2));
    }
    outColor = vec4(result, 1.0);
}
