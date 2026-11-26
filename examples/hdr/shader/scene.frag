#version 450

// Four point lights with inverse-square falloff, exactly like 6.hdr.  The key
// property is that radiance is allowed to leave the shader greater than 1:
// the back light is 200, so a naive RGBA8 target would clip it to white before
// tone mapping ever sees it.
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) flat in vec4 inMaterial;

layout(std140, set = 0, binding = 0) uniform HdrBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 params;
} ubo;

layout(location = 0) out vec4 outColor;

const vec3 LIGHT_POS[4] = vec3[4](
    vec3(0.0, 0.0, -35.0),
    vec3(-1.4, -1.9, -2.0),
    vec3(0.0, -1.8, -8.0),
    vec3(0.8, -1.7, -5.0));
const vec3 LIGHT_COLOR[4] = vec3[4](
    vec3(200.0, 200.0, 200.0),
    vec3(0.10, 0.0, 0.0),
    vec3(0.0, 0.0, 0.20),
    vec3(0.0, 0.10, 0.0));

void main()
{
    // Emissive geometry stores its radiance directly in the material colour.
    if (inMaterial.a > 0.5) {
        outColor = vec4(inMaterial.rgb, 1.0);
        return;
    }

    vec3 base = inMaterial.rgb;
    // A checker on the tunnel walls makes the exposure change measurable even
    // when the whole surface is washed out by the 200-unit back light.
    const vec2 cell = floor(inWorldPos.xz * 0.7 + inWorldPos.y * 0.7);
    base *= 0.72 + 0.28 * mod(cell.x + cell.y, 2.0);

    // The tunnel cube is seen from the inside, so its outward normals point
    // away from every light.  The reference flips them with invertedNormals;
    // params.w carries that flag here.
    const vec3 normal = ubo.params.w > 0.5 ? -normalize(inNormal) : normalize(inNormal);
    const vec3 viewDir = normalize(ubo.eye.xyz - inWorldPos);
    vec3 radiance = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        const vec3 toLight = LIGHT_POS[i] - inWorldPos;
        const float distanceSq = max(dot(toLight, toLight), 0.01);
        const vec3 lightDir = normalize(toLight);
        const float diffuse = max(dot(normal, lightDir), 0.0);
        const vec3 halfway = normalize(lightDir + viewDir);
        const float specular = pow(max(dot(normal, halfway), 0.0), 32.0);
        radiance += (diffuse + specular) * LIGHT_COLOR[i] / distanceSq;
    }
    outColor = vec4(base * radiance, 1.0);
}
