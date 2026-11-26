#version 450
struct MaterialParams { uint baseColorTexId; uint specularTexId; float roughness; float metallic; };
struct PointLight { vec4 position; vec4 color; vec4 attenuation; };
struct SpotLight { vec4 position; vec4 direction; vec4 color; vec4 params; };

layout(set = 0, binding = 0) uniform sampler2D baseColorSampler;
layout(set = 0, binding = 1) uniform sampler2D specularSampler;
layout(set = 0, binding = 2) readonly buffer MaterialPool { MaterialParams params[]; } materials;
layout(set = 2, binding = 0) uniform EyePoint { vec4 position; } eye;
layout(set = 2, binding = 1) uniform LightSource {
    vec4 position; vec4 ambient; vec4 diffuse; vec4 specular;
} light;
layout(set = 2, binding = 2) uniform DirectionalLight {
    vec4 direction; vec4 color;
} directional;
layout(set = 2, binding = 3) readonly buffer PointLights { PointLight lights[]; } pointLights;
layout(set = 2, binding = 4) uniform SpotLightBlock { SpotLight light; } spot;
// x > 0.5 selects Blinn-Phong, y = Phong exponent, z = Blinn exponent.
layout(set = 2, binding = 5) uniform LightingParams { vec4 params; } lighting;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) flat in int fragInstance;
layout(location = 0) out vec4 outColor;

vec3 phong(vec3 N, vec3 L, vec3 V, vec3 lightColor, vec3 albedo, float specStrength) {
    float diff = max(dot(N, L), 0.0);
    float spec;
    if (lighting.params.x > 0.5) {
        // Blinn-Phong: the reference's Blinn branch uses a half vector and a
        // higher exponent (8 -> 32) so the wider lobe stays tight.  This
        // example keeps Phong at 32 by default and uses 64 for Blinn; both
        // exponents are runtime parameters for A/B comparison.
        vec3 H = normalize(L + V);
        spec = pow(max(dot(N, H), 0.0), lighting.params.z) * specStrength;
    } else {
        vec3 R = reflect(-L, N);
        spec = pow(max(dot(R, V), 0.0), lighting.params.y) * specStrength;
    }
    return lightColor * (albedo * diff + vec3(spec));
}

void main() {
    MaterialParams mat = materials.params[fragInstance % 2];
    vec3 albedo = texture(baseColorSampler, fragTexCoord).rgb;
    float specStrength = texture(specularSampler, fragTexCoord).r;
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(eye.position.xyz - fragPos);

    vec3 color = vec3(0.05) * albedo;
    color += phong(N, normalize(light.position.xyz - fragPos), V, light.diffuse.rgb, albedo, specStrength);
    color += phong(N, normalize(-directional.direction.xyz), V, directional.color.rgb, albedo, specStrength);

    for (int i = 0; i < 4; ++i) {
        PointLight p = pointLights.lights[i];
        vec3 L = p.position.xyz - fragPos;
        float d = length(L);
        float att = 1.0 / (1.0 + p.attenuation.x * d + p.attenuation.y * d * d);
        color += phong(N, normalize(L), V, p.color.rgb * att, albedo, specStrength);
    }

    vec3 Ls = spot.light.position.xyz - fragPos;
    float theta = dot(normalize(Ls), normalize(spot.light.direction.xyz));
    if (theta > spot.light.params.y) {
        color += phong(N, normalize(Ls), V, spot.light.color.rgb, albedo, specStrength);
    }
    outColor = vec4(color, 1.0);
}
