#version 450
// Subpass 1 (deferred composition): reads the G-Buffer as input attachments and
// shades the light array.  The light count is a specialization constant so the
// same SPIR-V can drive a different light budget without recompiling.

layout(input_attachment_index = 0, binding = 0) uniform subpassInput samplerPosition;
layout(input_attachment_index = 1, binding = 1) uniform subpassInput samplerNormal;
layout(input_attachment_index = 2, binding = 2) uniform subpassInput samplerAlbedo;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(constant_id = 0) const int NUM_LIGHTS = 64;

struct Light
{
    vec4 position;
    vec3 color;
    float radius;
};

layout(binding = 3) uniform UBO
{
    vec4 viewPos;
    Light lights[NUM_LIGHTS];
} ubo;

void main()
{
    vec3 fragPos = subpassLoad(samplerPosition).rgb;
    vec3 normal = subpassLoad(samplerNormal).rgb;
    vec4 albedo = subpassLoad(samplerAlbedo);

    // Low ambient: the coloured pools need a dark background to read as lights.
    const float ambient = 0.10;
    vec3 fragColor = albedo.rgb * ambient;

    vec3 V = normalize(ubo.viewPos.xyz - fragPos);
    for (int i = 0; i < NUM_LIGHTS; ++i)
    {
        vec3 L = ubo.lights[i].position.xyz - fragPos;
        float dist = length(L);
        L = normalize(L);
        float atten = ubo.lights[i].radius / (dist * dist + 1.0);
        float NdotL = max(0.0, dot(normal, L));
        fragColor += ubo.lights[i].color * albedo.rgb * NdotL * atten;
    }

    // 64 lights summed in a small volume easily exceed 1.0; Reinhard keeps the
    // lit regions readable instead of clipping them to flat white.
    fragColor = fragColor / (fragColor + vec3(1.0));
    outColor = vec4(fragColor, 1.0);
}
