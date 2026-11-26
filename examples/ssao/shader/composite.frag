#version 450

// Deferred lighting pass (9.ssao_lighting.fs), in view space.  AO modulates only
// the ambient term; diffuse/specular are unaffected.  A point light in view
// space with the reference's colour and attenuation, specular exponent 8.
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gPosition;
layout(set = 0, binding = 1) uniform sampler2D gNormal;
layout(set = 0, binding = 2) uniform sampler2D gAlbedo;
layout(set = 0, binding = 3) uniform sampler2D ssao;

layout(std140, set = 1, binding = 0) uniform LightBlock
{
    vec4 lightPosView; // xyz = light position in view space
    vec4 lightColor;
    vec4 params;       // x = ssao enable, y = show AO only
} ubo;

void main()
{
    const vec4 positionSample = texture(gPosition, inUV);
    if (positionSample.w < 0.5) {
        outColor = vec4(vec3(0.05), 1.0);
        return;
    }
    const vec3 fragPos = positionSample.xyz;
    const vec3 normal = normalize(texture(gNormal, inUV).xyz);
    const vec3 albedo = texture(gAlbedo, inUV).rgb;
    const float rawAO = texture(ssao, inUV).r;
    const float ao = ubo.params.x > 0.5 ? rawAO : 1.0;

    if (ubo.params.y > 0.5) {
        outColor = vec4(vec3(rawAO), 1.0);
        return;
    }

    const vec3 ambient = vec3(0.3) * albedo * ao;
    const vec3 lightDir = normalize(ubo.lightPosView.xyz - fragPos);
    const float diff = max(dot(normal, lightDir), 0.0);
    const vec3 diffuse = diff * ubo.lightColor.rgb * albedo;
    const vec3 viewDir = normalize(-fragPos); // camera at view-space origin
    const vec3 halfway = normalize(lightDir + viewDir);
    const float spec = pow(max(dot(normal, halfway), 0.0), 8.0);
    const vec3 specular = spec * ubo.lightColor.rgb;

    const float distance = length(ubo.lightPosView.xyz - fragPos);
    const float attenuation = 1.0 / (1.0 + 0.09 * distance + 0.032 * distance * distance);

    outColor = vec4(ambient + (diffuse + specular) * attenuation, 1.0);
}
