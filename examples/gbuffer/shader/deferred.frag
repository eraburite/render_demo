#version 450
layout(set = 4, binding = 0) uniform sampler2D gPosition;
layout(set = 4, binding = 1) uniform sampler2D gNormal;
layout(set = 4, binding = 2) uniform sampler2D gAlbedo;
layout(set = 2, binding = 0) uniform EyePoint { vec4 position; } eye;
layout(set = 2, binding = 1) uniform LightSource {
    vec4 position; vec4 ambient; vec4 diffuse; vec4 specular;
} light;
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 worldPos = texture(gPosition, fragTexCoord).xyz;
    vec3 N = normalize(texture(gNormal, fragTexCoord).xyz);
    vec3 albedo = texture(gAlbedo, fragTexCoord).rgb;
    vec3 L = normalize(light.position.xyz - worldPos);
    vec3 V = normalize(eye.position.xyz - worldPos);
    float diff = max(dot(N, L), 0.0);
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, V), 0.0), 32.0);
    vec3 color = light.ambient.rgb * albedo + light.diffuse.rgb * diff * albedo + light.specular.rgb * spec;
    outColor = vec4(color, 1.0);
}
