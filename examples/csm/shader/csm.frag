#version 450

// Main (lit) pass fragment shader: Blinn-Phong plus cascaded shadow lookup.
// The lighting terms are the reference implementation's, unchanged, so the only
// visible difference between this and a no-shadow run is the shadow itself.
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) flat in vec4 inMaterial;

layout(set = 2, binding = 0) uniform sampler2DArray shadowMap;

layout(std140, set = 0, binding = 0) uniform CsmBlock
{
    mat4 lightSpaceMatrices[5];
    mat4 view;
    mat4 viewProj;
    vec4 cascadePlaneDistances;
    vec4 lightDir;
    vec4 eye;
    vec4 flags; // x = debug cascade, y = layer count, z = shadow enable, w = 1 / resolution
} ubo;

layout(location = 0) out vec4 outColor;

const int kMaxCascades = 5;

// Which cascade covers this fragment.  Selection is by *view-space* depth, not
// by distance to the eye, because the split planes are perpendicular to the
// view direction: a fragment off to the side is closer to the eye than it is
// deep, and picking by distance would drop it into too fine a cascade.
int selectCascade(float viewDepth)
{
    const int layerCount = int(ubo.flags.y + 0.5);
    for (int i = 0; i < kMaxCascades - 1; ++i) {
        if (i >= layerCount - 1) {
            break;
        }
        if (viewDepth < ubo.cascadePlaneDistances[i]) {
            return i;
        }
    }
    return layerCount - 1;
}

// Light clip -> [0,1] texture space for one cascade.
vec3 lightProject(int layer)
{
    const vec4 lightClip = ubo.lightSpaceMatrices[layer] * vec4(inWorldPos, 1.0);
    vec3 proj = lightClip.xyz / lightClip.w;

    // The light projection is built with a Vulkan depth range (z in [0,1]), but
    // x/y are still NDC in [-1,1] and need the usual remap.  The reference
    // multiplies all three components by 0.5 and adds 0.5 because its depth
    // range is [-1,1]; doing that here would halve every depth value and make
    // the whole scene read as lit.
    proj.xy = proj.xy * 0.5 + 0.5;
    return proj;
}

// 9-tap PCF against one cascade layer.  Returns 0 when lit, 1 when fully
// shadowed -- the same convention the reference uses, so the lighting line
// below stays readable.
float shadowFactor(int layer, vec3 proj)
{
    const float currentDepth = proj.z;
    // Past the light frustum' far plane: outside any shadow, keep it lit.
    if (currentDepth > 1.0) {
        return 0.0;
    }

    const vec3 normal = normalize(inNormal);
    const vec3 lightDirection = normalize(ubo.lightDir.xyz);
    float bias = max(0.05 * (1.0 - dot(normal, lightDirection)), 0.005);
    // The bias has to grow with the cascade's extent: one shadow texel covers a
    // much bigger world distance in the far cascade, so a fixed bias either
    // over-biases cascade 0 or leaves acne in cascade 4.
    const int layerCount = int(ubo.flags.y + 0.5);
    const float splitDistance = (layer == layerCount - 1) ? ubo.lightDir.w : ubo.cascadePlaneDistances[layer];
    bias *= 1.0 / (splitDistance * 0.5);

    const vec2 texelSize = vec2(ubo.flags.w);
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            const float pcfDepth =
                texture(shadowMap, vec3(proj.xy + vec2(x, y) * texelSize, float(layer))).r;
            shadow += (currentDepth - bias) > pcfDepth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

void main()
{
    vec3 base = inMaterial.rgb;
    if (inMaterial.a > 0.5) {
        // Procedural checker instead of a floor texture: the shadow edges stay
        // measurable against it, and the example needs no image asset.
        const vec2 cell = floor(inWorldPos.xz * 0.5);
        base *= 0.72 + 0.28 * mod(cell.x + cell.y, 2.0);
    }

    const vec3 normal = normalize(inNormal);
    const vec3 lightDirection = normalize(ubo.lightDir.xyz);
    const vec3 lightColor = vec3(0.3);

    const vec3 ambient = 0.3 * base;
    const float diff = max(dot(lightDirection, normal), 0.0);
    const vec3 diffuse = diff * lightColor;
    const vec3 viewDir = normalize(ubo.eye.xyz - inWorldPos);
    const vec3 halfwayDir = normalize(lightDirection + viewDir);
    const float spec = pow(max(dot(normal, halfwayDir), 0.0), 64.0);
    const vec3 specular = spec * lightColor;

    // Always work out the cascade and its projection.  --no-shadow only skips
    // the comparison against the map; the depth views below still have to show
    // which cascade a fragment belongs to and what is stored there, otherwise
    // they silently turn into a wall of black.
    const float viewDepth = abs((ubo.view * vec4(inWorldPos, 1.0)).z);
    const int layer = selectCascade(viewDepth);
    const vec3 proj = lightProject(layer);
    const float shadow = ubo.flags.z > 0.5 ? shadowFactor(layer, proj) : 0.0;

    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular)) * base;

    // Debug views.  These are not decoration: a shadow map that is silently
    // empty, over-biased or sampled at the wrong layer all look the same in the
    // lit image (flat, or uniformly dark), and each of them has a different
    // signature in these three channels.
    if (ubo.flags.x > 4.5) {
        // Signed difference fragment-minus-map, 0.5 grey = equal.  This is the
        // probe that distinguishes "the lookup is offset" from "the bias is too
        // small": an offset shows as a flat shift, a bias problem as acne.
        const float mapDepth = texture(shadowMap, vec3(proj.xy, float(layer))).r;
        outColor = vec4(proj.z, mapDepth, 0.0, 1.0); // R = fragment, G = map
        return;
    }
    if (ubo.flags.x > 3.5) {
        // Depth this fragment projects to, in the cascade it selected.
        outColor = vec4(vec3(proj.z), 1.0);
        return;
    }
    if (ubo.flags.x > 2.5) {
        // Depth stored in the shadow map at the same texel.
        outColor = vec4(vec3(texture(shadowMap, vec3(proj.xy, float(layer))).r), 1.0);
        return;
    }
    if (ubo.flags.x > 1.5) {
        // Raw shadow term: white means "fully occluded", black "fully lit".
        outColor = vec4(vec3(shadow), 1.0);
        return;
    }
    if (ubo.flags.x > 0.5) {
        // Tint by cascade so the splits are visible without a debug overlay:
        // red -> yellow -> green -> blue -> purple, near to far.
        const vec3 palette[5] = vec3[5](
            vec3(0.90, 0.20, 0.20), vec3(0.90, 0.70, 0.20), vec3(0.20, 0.80, 0.30),
            vec3(0.20, 0.50, 0.90), vec3(0.70, 0.30, 0.90));
        lighting = mix(lighting, palette[layer], 0.55);
    }

    outColor = vec4(lighting, 1.0);
}
