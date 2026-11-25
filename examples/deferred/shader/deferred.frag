#version 450
// Deferred lighting: sample the G-Buffer and accumulate Blinn-Phong from six
// point lights.  Lights are hard-coded here to keep the descriptor set minimal;
// only the eye position (set 2) and the three G-Buffer samplers (set 4) are used.
layout(set = 4, binding = 0) uniform sampler2D gPosition;
layout(set = 4, binding = 1) uniform sampler2D gNormal;
layout(set = 4, binding = 2) uniform sampler2D gAlbedo;
layout(set = 2, binding = 0) uniform EyePoint { vec4 position; } eye;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

const vec3 BACKGROUND = vec3(0.05, 0.06, 0.09);
const float AMBIENT   = 0.06;

// One point light: Blinn-Phong with distance attenuation radius/(d^2+1).
vec3 shade(vec3 P, vec3 N, vec3 V, vec3 albedo, vec3 lightPos, vec3 lightColor, float radius)
{
    vec3 toLight = lightPos - P;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-4);

    float atten = radius / (dist * dist + 1.0);
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = lightColor * albedo * NdotL * atten;

    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    vec3 specular = lightColor * pow(NdotH, 48.0) * 0.35 * atten;

    return diffuse + specular;
}

void main()
{
    vec4 posSample = texture(gPosition, fragTexCoord);
    vec4 normSample = texture(gNormal, fragTexCoord);
    vec4 albedoSample = texture(gAlbedo, fragTexCoord);

    // The G-Buffer clear color has alpha 0 while mrt.frag always writes alpha 1,
    // so alpha is a reliable "did geometry cover this pixel" mask.  Without it
    // cleared texels would feed normalize(vec3(0)) into the light math (NaN).
    if (normSample.a < 0.5) {
        outColor = vec4(BACKGROUND, 1.0);
        return;
    }

    vec3 worldPos = posSample.xyz;
    vec3 N = normalize(normSample.xyz);
    vec3 albedo = albedoSample.rgb;
    vec3 V = normalize(eye.position.xyz - worldPos);

    vec3 color = albedo * AMBIENT;
    color += shade(worldPos, N, V, albedo, vec3( 2.2,  1.5,  2.0), vec3(1.00, 0.35, 0.30), 3.0);
    color += shade(worldPos, N, V, albedo, vec3(-2.2,  1.5,  2.0), vec3(0.30, 0.60, 1.00), 3.0);
    color += shade(worldPos, N, V, albedo, vec3( 2.2, -1.5, -2.0), vec3(1.00, 0.80, 0.25), 3.0);
    color += shade(worldPos, N, V, albedo, vec3(-2.2, -1.5, -2.0), vec3(0.40, 1.00, 0.55), 3.0);
    color += shade(worldPos, N, V, albedo, vec3( 0.0,  2.5, -1.0), vec3(0.80, 0.40, 1.00), 3.0);
    color += shade(worldPos, N, V, albedo, vec3( 0.0, -2.0,  2.5), vec3(1.00, 1.00, 1.00), 3.0);

    outColor = vec4(color, 1.0);
}
