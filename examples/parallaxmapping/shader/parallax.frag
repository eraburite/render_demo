#version 450

// Parallax mapping fragment stage (reference: data/shaders/glsl/parallaxmapping/
// parallax.frag).  All four reference paths are reproduced: 0 = colour only,
// 2 = parallax mapping, 3 = steep parallax mapping, 4 = parallax occlusion
// mapping.  The mapping scalars travel in the push constant block (the shared
// per-frame table has no free slot), and the eye position comes from the
// fragment-stage UBO declared by the frame table.

layout(set = 0, binding = 0) uniform sampler2D sColorMap;
layout(set = 0, binding = 1) uniform sampler2D sNormalHeightMap;

layout(set = 2, binding = 0) uniform SceneBlock {
    vec4 eyePosition;
} scene;

layout(push_constant) uniform PushBlock {
    mat4 model;
    vec4 lightPos;
    float heightScale;
    float parallaxBias;
    float numLayers;
    int mappingMode;
} push;

layout(location = 0) in vec2 inUV;
layout(location = 1) in mat3 inTBN;
layout(location = 4) in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;

// Height is stored in the alpha channel of the normal map, with 1 = deepest.
float sampleHeight(vec2 uv)
{
    return 1.0 - textureLod(sNormalHeightMap, uv, 0.0).a;
}

vec2 parallaxMapping(vec2 uv, vec3 viewDir)
{
    const float height = sampleHeight(uv);
    const vec2 p = viewDir.xy * (height * (push.heightScale * 0.5) + push.parallaxBias) / viewDir.z;
    return uv - p;
}

vec2 steepParallaxMapping(vec2 uv, vec3 viewDir)
{
    const float layerDepth = 1.0 / push.numLayers;
    float currLayerDepth = 0.0;
    const vec2 deltaUV = viewDir.xy * push.heightScale / (viewDir.z * push.numLayers);
    vec2 currUV = uv;
    float height = sampleHeight(currUV);
    for (int i = 0; i < int(push.numLayers); ++i) {
        currLayerDepth += layerDepth;
        currUV -= deltaUV;
        height = sampleHeight(currUV);
        if (height < currLayerDepth) {
            break;
        }
    }
    return currUV;
}

vec2 parallaxOcclusionMapping(vec2 uv, vec3 viewDir)
{
    const float layerDepth = 1.0 / push.numLayers;
    float currLayerDepth = 0.0;
    const vec2 deltaUV = viewDir.xy * push.heightScale / (viewDir.z * push.numLayers);
    vec2 currUV = uv;
    float height = sampleHeight(currUV);
    for (int i = 0; i < int(push.numLayers); ++i) {
        currLayerDepth += layerDepth;
        currUV -= deltaUV;
        height = sampleHeight(currUV);
        if (height < currLayerDepth) {
            break;
        }
    }
    // Interpolate between the two straddling layers for a smooth surface.
    const vec2 prevUV = currUV + deltaUV;
    const float nextDepth = height - currLayerDepth;
    const float prevDepth = 1.0 - textureLod(sNormalHeightMap, prevUV, 0.0).a - currLayerDepth + layerDepth;
    return mix(currUV, prevUV, nextDepth / (nextDepth - prevDepth));
}

void main()
{
    // World -> tangent space: the TBN columns are world-space axes, so the
    // inverse rotation is the transpose.
    const mat3 worldToTangent = transpose(inTBN);
    const vec3 V = normalize(worldToTangent * (scene.eyePosition.xyz - inWorldPos));

    if (push.mappingMode == 0) {
        outColor = texture(sColorMap, inUV);
        return;
    }

    vec2 uv = inUV;
    switch (push.mappingMode) {
        case 2:
            uv = parallaxMapping(inUV, V);
            break;
        case 3:
            uv = steepParallaxMapping(inUV, V);
            break;
        case 4:
            uv = parallaxOcclusionMapping(inUV, V);
            break;
        default:
            break;
    }

    // Sample before the (potential) discard: an implicit-derivative sample in
    // non-uniform control flow would be undefined.
    const vec3 normalHeightMapLod = textureLod(sNormalHeightMap, uv, 0.0).rgb;
    const vec3 color = texture(sColorMap, uv).rgb;

    // Parallax offsets push UVs outside the tile; drop those fragments instead
    // of stretching the border texel across the plane.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        discard;
    }

    const vec3 N = normalize(normalHeightMapLod * 2.0 - 1.0);
    const vec3 L = normalize(worldToTangent * (push.lightPos.xyz - inWorldPos));
    const vec3 H = normalize(L + V);

    const vec3 ambient = 0.2 * color;
    const vec3 diffuse = max(dot(L, N), 0.0) * color;
    const vec3 specular = vec3(0.15) * pow(max(dot(N, H), 0.0), 32.0);

    outColor = vec4(ambient + diffuse + specular, 1.0);
}
