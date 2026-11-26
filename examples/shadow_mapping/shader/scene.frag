#version 450

// Manual depth comparison against one 2D shadow map: the same algorithm as
// LearnOpenGL 3.1.3, with an explicit Vulkan [0,1] depth range.  Unlike the CSM
// example there is no cascade selection, no array layer and no geometry shader.
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) flat in vec4 inMaterial;

layout(set = 2, binding = 0) uniform sampler2D shadowMap;

layout(std140, set = 0, binding = 0) uniform ShadowBlock
{
    mat4 lightSpaceMatrix;
    mat4 viewProj;
    vec4 lightDir;
    vec4 eye;
    vec4 flags; // x = debug view, y = bias, z = PCF, w = shadow enable
} ubo;

layout(location = 0) out vec4 outColor;

// Light clip -> [0,1] texture space.  x/y are NDC in [-1,1] and need the usual
// remap; z is already Vulkan depth [0,1] (glm::ortho's [-1,1] range was
// converted in test30.cpp), so it must NOT be remapped again.
vec3 lightProject(vec3 worldPos)
{
    const vec4 lightClip = ubo.lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 proj = lightClip.xyz / lightClip.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    return proj;
}

float shadowFactor(vec3 proj, vec3 normal, vec3 lightDirection)
{
    if (proj.z > 1.0) {
        return 0.0; // outside the light frustum: the white border keeps it lit
    }

    float bias = 0.005;
    if (ubo.flags.y > 0.5) {
        // Slope-scaled bias from the reference: max(0.05 * (1 - dot(N, L)), 0.005).
        bias = max(0.05 * (1.0 - dot(normal, lightDirection)), 0.005);
    }

    float shadow = 0.0;
    if (ubo.flags.z > 0.5) {
        // 3x3 software PCF, one texel radius, same as the reference.
        // eye.w carries 1 / shadow-map resolution so --shadow-res stays honest.
        const vec2 texelSize = vec2(ubo.eye.w);
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                const float pcfDepth = texture(shadowMap, proj.xy + vec2(x, y) * texelSize).r;
                shadow += (proj.z - bias) > pcfDepth ? 1.0 : 0.0;
            }
        }
        shadow /= 9.0;
    } else {
        shadow = (proj.z - bias) > texture(shadowMap, proj.xy).r ? 1.0 : 0.0;
    }
    return shadow;
}

void main()
{
    vec3 base = inMaterial.rgb;
    if (inMaterial.a > 0.5) {
        // Procedural checker instead of a wood texture: the example needs no
        // image asset and shadow edges stay measurable against the grid.
        const vec2 cell = floor(inWorldPos.xz * 0.5);
        base *= 0.72 + 0.28 * mod(cell.x + cell.y, 2.0);
    }

    const vec3 normal = normalize(inNormal);
    // The reference uses a point light position, so the direction is per
    // fragment even though the shadow frustum is orthographic.
    const vec3 lightDirection = normalize(ubo.lightDir.xyz - inWorldPos);
    const vec3 lightColor = vec3(1.0);
    const vec3 ambient = 0.3 * lightColor;
    const float diff = max(dot(lightDirection, normal), 0.0);
    const vec3 diffuse = diff * lightColor;
    const vec3 viewDir = normalize(ubo.eye.xyz - inWorldPos);
    const vec3 halfwayDir = normalize(lightDirection + viewDir);
    const float spec = pow(max(dot(normal, halfwayDir), 0.0), 64.0);
    const vec3 specular = spec * lightColor;

    // Always project: the debug views must work even with shadows disabled.
    const vec3 proj = lightProject(inWorldPos);
    const float shadow = ubo.flags.w > 0.5 ? shadowFactor(proj, normal, lightDirection) : 0.0;
    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular)) * base;

    // 0 = lit, 1 = shadow term, 2 = stored depth, 3 = fragment/map difference,
    // 4 = fragment depth.  Kept as the same style of probe as examples/csm.
    if (ubo.flags.x > 3.5) {
        outColor = vec4(vec3(proj.z), 1.0);
    } else if (ubo.flags.x > 2.5) {
        const float mapDepth = texture(shadowMap, proj.xy).r;
        outColor = vec4(proj.z, mapDepth, 0.0, 1.0);
    } else if (ubo.flags.x > 1.5) {
        outColor = vec4(vec3(texture(shadowMap, proj.xy).r), 1.0);
    } else if (ubo.flags.x > 0.5) {
        outColor = vec4(vec3(shadow), 1.0);
    } else {
        outColor = vec4(lighting, 1.0);
    }
}
