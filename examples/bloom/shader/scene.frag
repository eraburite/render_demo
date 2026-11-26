#version 450

// MRT geometry pass of 7.bloom.  Attachment 1 is the reference's BrightColor:
// Rec.709 luminance above 1.0 is copied, everything else is cleared to black.
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) flat in vec4 inMaterial;

layout(std140, set = 0, binding = 0) uniform BloomBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 params;
} ubo;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outBright;

const vec3 LIGHT_POS[4] = vec3[4](
    vec3(0.0, 0.5, 1.5),
    vec3(-4.0, 0.5, -3.0),
    vec3(3.0, 0.5, 1.0),
    vec3(-0.8, 2.4, -1.0));
const vec3 LIGHT_COLOR[4] = vec3[4](
    vec3(5.0, 5.0, 5.0),
    vec3(10.0, 0.0, 0.0),
    vec3(0.0, 0.0, 15.0),
    vec3(0.0, 5.0, 0.0));

void main()
{
    vec3 radiance;
    if (inMaterial.a > 0.5) {
        // The light boxes are the emitters: their colour goes straight into
        // both targets and therefore drives the threshold.
        radiance = inMaterial.rgb;
    } else {
        vec3 base = inMaterial.rgb;
        const vec2 cell = floor(inWorldPos.xz * 0.5 + inWorldPos.y * 0.25);
        base *= 0.72 + 0.28 * mod(cell.x + cell.y, 2.0);
        const vec3 normal = normalize(inNormal);
        const vec3 viewDir = normalize(ubo.eye.xyz - inWorldPos);
        radiance = vec3(0.0);
        for (int i = 0; i < 4; ++i) {
            const vec3 toLight = LIGHT_POS[i] - inWorldPos;
            const float distanceSq = max(dot(toLight, toLight), 0.01);
            const vec3 lightDir = normalize(toLight);
            const float diffuse = max(dot(normal, lightDir), 0.0);
            const vec3 halfway = normalize(lightDir + viewDir);
            const float specular = pow(max(dot(normal, halfway), 0.0), 32.0);
            radiance += (diffuse + specular) * LIGHT_COLOR[i] / distanceSq;
        }
        radiance *= base;
    }

    outColor = vec4(radiance, 1.0);
    const float brightness = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    outBright = ubo.params.x > 0.5 && brightness > 1.0 ? vec4(radiance, 1.0) : vec4(0.0, 0.0, 0.0, 0.0);
}
