#version 450
layout(set = 4, binding = 0) uniform sampler2D gPosition;
layout(set = 4, binding = 1) uniform sampler2D gNormal;
layout(set = 4, binding = 2) uniform sampler2D gAlbedo;
layout(set = 2, binding = 0) uniform EyePoint { vec4 position; } eye;
layout(set = 2, binding = 1) uniform LightSource {
    vec4 position; vec4 ambient; vec4 diffuse; vec4 specular;
} light;

// Debug View switch (see examples/common/DebugViewMode.h).  The field order and
// types match DebugViewPushConstant exactly: eight 4-byte scalars = 32 bytes.
layout(push_constant) uniform DebugView {
    uint mode;
    float nearPlane;
    float farPlane;
    float referenceScale;
    float viewportWidth;
    float viewportHeight;
    float contentWidth;
    float contentHeight;
} debugView;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// Keep in sync with the clear colour used by the other examples.
const vec3 BACKGROUND = vec3(0.05, 0.06, 0.09);

void main() {
    vec4 positionSample = texture(gPosition, fragTexCoord);
    vec4 normalSample = texture(gNormal, fragTexCoord);
    vec4 albedoSample = texture(gAlbedo, fragTexCoord);

    // The G-Buffer clear colour has alpha 0 while mrt.frag always writes alpha 1,
    // so alpha is a reliable "did geometry cover this pixel" mask.  Without it the
    // cleared pixels would feed normalize(vec3(0)) into the light math and produce
    // NaNs across the background.
    if (normalSample.a < 0.5) {
        outColor = vec4(BACKGROUND, 1.0);
        return;
    }

    vec3 worldPos = positionSample.xyz;
    vec3 N = normalize(normalSample.xyz);
    vec3 albedo = albedoSample.rgb;

    // ---- Debug View: dump an intermediate channel, skip lighting ----------
    if (debugView.mode == 1u) { outColor = vec4(albedo, 1.0); return; }
    if (debugView.mode == 2u) { outColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (debugView.mode == 3u) {
        outColor = vec4(clamp(worldPos / debugView.referenceScale * 0.5 + 0.5, 0.0, 1.0), 1.0);
        return;
    }
    if (debugView.mode == 4u) {
        // Linear distance to the eye reconstructed from the position channel, so
        // the depth attachment never has to be stored or sampled.  Near = bright.
        float distance = length(worldPos - eye.position.xyz);
        float span = max(debugView.farPlane - debugView.nearPlane, 1e-4);
        float t = clamp((distance - debugView.nearPlane) / span, 0.0, 1.0);
        outColor = vec4(vec3(1.0 - t), 1.0);
        return;
    }

    vec3 L = normalize(light.position.xyz - worldPos);
    vec3 V = normalize(eye.position.xyz - worldPos);
    float diff = max(dot(N, L), 0.0);
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, V), 0.0), 32.0);

    if (debugView.mode == 5u) {
        // Lighting contribution with albedo forced to white: separates "the light
        // is wrong" from "the albedo is wrong".
        vec3 lightingOnly = light.ambient.rgb + light.diffuse.rgb * diff + light.specular.rgb * spec;
        outColor = vec4(lightingOnly, 1.0);
        return;
    }

    vec3 color = light.ambient.rgb * albedo + light.diffuse.rgb * diff * albedo + light.specular.rgb * spec;
    outColor = vec4(color, 1.0);
}
