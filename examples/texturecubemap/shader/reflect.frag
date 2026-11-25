#version 450

layout(set = 0, binding = 0) uniform samplerCube samplerColor;

layout(push_constant) uniform PushBlock {
    mat4 model;
    float lodBias;
} push;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inViewVec;
layout(location = 3) in vec3 inLightVec;

layout(location = 0) out vec4 outColor;

void main()
{
    // Both the fragment position and the normal are already in world space, and
    // the environment map is a world-space cube, so the reflection vector needs
    // no model-space round trip.  (The reference example carries an inverseModel
    // because it pre-transforms vertices into world space.)  xy is flipped to
    // match the skybox shader's GL -> Vulkan cube orientation.
    const vec3 cI = normalize(inWorldPos);
    vec3 cR = reflect(cI, normalize(inNormal));
    cR.xy *= -1.0;

    const vec4 color = texture(samplerColor, cR, push.lodBias);

    const vec3 N = normalize(inNormal);
    const vec3 L = normalize(inLightVec);
    const vec3 V = normalize(inViewVec);
    const vec3 R = reflect(-L, N);

    const vec3 ambient = vec3(0.5) * color.rgb;
    const vec3 diffuse = max(dot(N, L), 0.0) * vec3(1.0);
    const vec3 specular = pow(max(dot(R, V), 0.0), 16.0) * vec3(0.5);

    outColor = vec4(ambient + diffuse * color.rgb + specular, 1.0);
}
