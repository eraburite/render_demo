#version 450

// Lit pass: Blinn-Phong with a point-light shadow stored in a 6-layer depth
// array (one layer per cube face).  The reference samples a samplerCube with
// the raw fragToLight direction; Vulkan has no hardware cube-depth attachment
// here, so the same 6 face matrices the depth pass rasterized with are used to
// turn a direction into (layer, uv).  Reprojecting with the identical matrix is
// what keeps the lookup consistent with the stored depth -- replicating the
// GL cubemap sc/tc convention by hand would be a second, independent source of
// truth that could silently mirror a face.
layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) flat in vec4 inMaterial;

layout(set = 2, binding = 0) uniform sampler2DArray shadowMap;

layout(std140, set = 0, binding = 0) uniform PointBlock
{
    mat4 faceMatrices[6];
    vec4 lightPosFar;  // xyz = light position, w = far plane
    mat4 viewProj;
    vec4 eye;
    vec4 flags;        // x = debug view, y = shadow enable, z = soft(0/1), w = 1/resolution
} ubo;

layout(location = 0) out vec4 outColor;

const int kFaceCount = 6;

// Fixed-direction grid disk from 3.2.2.point_shadows_soft (the reference calls
// it a Poisson disk, but the 20 vectors are a deterministic lattice).
const vec3 kGridDisk[20] = vec3[20](
    vec3( 1.0,  1.0,  1.0), vec3( 1.0, -1.0,  1.0), vec3(-1.0, -1.0,  1.0), vec3(-1.0,  1.0,  1.0),
    vec3( 1.0,  1.0, -1.0), vec3( 1.0, -1.0, -1.0), vec3(-1.0, -1.0, -1.0), vec3(-1.0,  1.0, -1.0),
    vec3( 1.0,  1.0,  0.0), vec3( 1.0, -1.0,  0.0), vec3(-1.0, -1.0,  0.0), vec3(-1.0,  1.0,  0.0),
    vec3( 1.0,  0.0,  1.0), vec3(-1.0,  0.0,  1.0), vec3( 1.0,  0.0, -1.0), vec3(-1.0,  0.0, -1.0),
    vec3( 0.0,  1.0,  1.0), vec3( 0.0, -1.0,  1.0), vec3( 0.0,  1.0, -1.0), vec3( 0.0, -1.0, -1.0));

// Face order must match the CPU dirs[] table: 0:+X 1:-X 2:+Y 3:-Y 4:+Z 5:-Z.
int faceFromDirection(vec3 dir)
{
    const vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) {
        return dir.x > 0.0 ? 0 : 1;
    }
    if (a.y >= a.z) {
        return dir.y > 0.0 ? 2 : 3;
    }
    return dir.z > 0.0 ? 4 : 5;
}

// Reprojection of a world point onto a face, using the SAME matrix the GS
// rasterized that face with.  clip.xy / clip.w is NDC in [-1,1]; the framebuffer
// and the sampled image share Vulkan's y-down convention, so *0.5 + 0.5 maps
// straight to texel coordinates without an extra mirror.
vec2 uvOnFace(vec3 worldPos, int face)
{
    const vec4 clip = ubo.faceMatrices[face] * vec4(worldPos, 1.0);
    return clip.xy / clip.w * 0.5 + 0.5;
}

float mapDepthAt(vec3 sampleWorldPos, out int face)
{
    face = faceFromDirection(sampleWorldPos - ubo.lightPosFar.xyz);
    return texture(shadowMap, vec3(uvOnFace(sampleWorldPos, face), float(face))).r;
}

float shadowFactor(vec3 fragToLight)
{
    const float far = ubo.lightPosFar.w;
    // The map stores distance / far, so multiply back to WORLD units before the
    // comparison: the reference biases (0.05 hard / 0.15 soft) are world-space
    // distances.  Comparing them against normalised [0,1] depth would scale the
    // bias by far (25x) and silently erase every shadow -- hard still shows a
    // little because its bias is smaller, soft loses all of it.
    const float currentDistance = length(fragToLight);
    if (currentDistance > far) {
        return 0.0;
    }

    const bool soft = ubo.flags.z > 0.5;
    const float bias = soft ? 0.15 : 0.05;

    if (!soft) {
        int face;
        const float closest = mapDepthAt(inWorldPos, face) * far;
        return (currentDistance - bias) > closest ? 1.0 : 0.0;
    }

    const float viewDistance = length(ubo.eye.xyz - inWorldPos);
    const float diskRadius = (1.0 + viewDistance / far) / 25.0;
    float shadow = 0.0;
    for (int i = 0; i < 20; ++i) {
        const vec3 sampleWorldPos = inWorldPos + kGridDisk[i] * diskRadius;
        int face;
        const float closest = mapDepthAt(sampleWorldPos, face) * far;
        shadow += (currentDistance - bias) > closest ? 1.0 : 0.0;
    }
    return shadow / 20.0;
}

void main()
{
    vec3 base = inMaterial.rgb;
    if (inMaterial.a > 0.5) {
        const vec2 cell = floor(inWorldPos.xz * 0.5);
        base *= 0.72 + 0.28 * mod(cell.x + cell.y, 2.0);
    }

    const vec3 normal = normalize(inNormal);
    const vec3 fragToLight = inWorldPos - ubo.lightPosFar.xyz;
    // Blinn-Phong, point light: light direction points from the fragment back
    // towards the light (the opposite of the cubemap sampling direction).
    const vec3 lightDir = normalize(-fragToLight);
    const vec3 viewDir = normalize(ubo.eye.xyz - inWorldPos);
    const vec3 halfwayDir = normalize(lightDir + viewDir);
    const vec3 lightColor = vec3(0.3);

    const vec3 ambient = 0.3 * base;
    const float diff = max(dot(lightDir, normal), 0.0);
    const vec3 diffuse = diff * lightColor;
    const float spec = pow(max(dot(normal, halfwayDir), 0.0), 64.0);
    const vec3 specular = spec * lightColor;

    // Always evaluate the lookup: --no-shadow only skips the comparison, so the
    // depth / map / diff debug views still show which face a fragment chose.
    const float shadow = ubo.flags.y > 0.5 ? shadowFactor(fragToLight) : 0.0;
    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular)) * base;

    const int debug = int(ubo.flags.x + 0.5);
    if (debug == 4) {
        int face;
        const float closest = mapDepthAt(inWorldPos, face);
        outColor = vec4(length(fragToLight) / ubo.lightPosFar.w, closest, 0.0, 1.0);
        return;
    }
    if (debug == 3) {
        int face;
        outColor = vec4(vec3(mapDepthAt(inWorldPos, face)), 1.0);
        return;
    }
    if (debug == 2) {
        outColor = vec4(vec3(length(fragToLight) / ubo.lightPosFar.w), 1.0);
        return;
    }
    if (debug == 1) {
        outColor = vec4(vec3(shadow), 1.0);
        return;
    }

    outColor = vec4(lighting, 1.0);
}
