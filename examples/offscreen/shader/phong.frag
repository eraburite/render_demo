#version 450
layout(set = 2, binding = 0) uniform EyePoint { vec4 position; } eye;
layout(set = 2, binding = 1) uniform LightSource {
    vec4 position; vec4 ambient; vec4 diffuse; vec4 specular;
} light;
layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(fragNormal);
    vec3 L = normalize(light.position.xyz - fragPos);
    vec3 Eye = normalize(-(fragPos - eye.position.xyz));
    float diff = max(dot(N, L), 0.0);
    vec3 R = reflect(-L, N);
    float spec = pow(max(dot(R, Eye), 0.0), 32.0);
    vec3 color = light.ambient.rgb + light.diffuse.rgb * diff + light.specular.rgb * spec;
    outColor = vec4(color, 1.0);
}
