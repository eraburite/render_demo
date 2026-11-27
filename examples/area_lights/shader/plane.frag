#version 450

// Linearly Transformed Cosines area lights, ported from
// LearnOpenGL/src/8.guest/2022/7.area_lights/2.multiple_area_lights.
//
// LTC replaces the GGX lobe by a cosine distribution warped with a 3x3 matrix M
// that depends only on (roughness, view angle).  The integral of a polygon over
// that distribution has a closed form, so an area light costs four edge
// integrals instead of an importance-sampled loop:
//
//   * LTC1 (64x64 RGBA32F) stores the inverse of M as four numbers
//   * LTC2 (64x64 RGBA32F) stores (F90 scale, Smith geometry term, -, horizon clip)
//
// Two lookups per shaded point, then per light: 4 vertex transforms, 4 edge
// integrals and one more LTC2 lookup for the horizon-clipped sphere.
//
// Diffuse is the same routine evaluated with M = identity (a pure cosine lobe).
layout(set = 0, binding = 0) uniform SceneBlock
{
    mat4 viewProj;
    vec4 eye;
    vec4 material; // rgb = albedo, w = roughness
    vec4 params;   // x = light count, y = global intensity, z = 1 diffuse only, w = 1 two-sided
} ubo;

layout(set = 1, binding = 0) uniform sampler2D LTC1;
layout(set = 1, binding = 1) uniform sampler2D LTC2;

struct AreaLight {
    vec4 p0;
    vec4 p1;
    vec4 p2;
    vec4 p3;
    vec4 color;  // rgb
    vec4 params; // x = intensity, y = two-sided
};
layout(std430, set = 2, binding = 0) readonly buffer AreaLights
{
    AreaLight lights[];
};

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec4 outColor;

const float LUT_SIZE = 64.0;
const float LUT_SCALE = (LUT_SIZE - 1.0) / LUT_SIZE;
const float LUT_BIAS = 0.5 / LUT_SIZE;

// Closed-form acos fit from the LTC reference implementation; the built-in
// acos() produces visible seams on the polygon edges.
vec3 integrateEdge(vec3 v1, vec3 v2)
{
    const float x = dot(v1, v2);
    const float y = abs(x);

    const float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    const float b = 3.4175940 + (4.1616724 + y) * y;
    const float v = a / b;

    const float thetaSinTheta = (x > 0.0) ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * thetaSinTheta;
}

// P is the shaded point in world space; Minv is LTC1's matrix or identity.
vec3 evaluate(vec3 N, vec3 V, vec3 P, mat3 Minv, vec3 points[4], bool twoSided)
{
    // Orthonormal basis around N.
    const vec3 T1 = normalize(V - N * dot(V, N));
    const vec3 T2 = cross(N, T1);
    Minv = Minv * transpose(mat3(T1, T2, N));

    // Polygon in the LTC space, relative to P.
    vec3 L[4];
    L[0] = Minv * (points[0] - P);
    L[1] = Minv * (points[1] - P);
    L[2] = Minv * (points[2] - P);
    L[3] = Minv * (points[3] - P);

    // Horizon clipping uses the tabulated sphere: is P behind the light?
    const vec3 dir = points[0] - P;
    const vec3 lightNormal = cross(points[1] - points[0], points[3] - points[0]);
    const bool behind = dot(dir, lightNormal) < 0.0;

    L[0] = normalize(L[0]);
    L[1] = normalize(L[1]);
    L[2] = normalize(L[2]);
    L[3] = normalize(L[3]);

    vec3 vsum = vec3(0.0);
    vsum += integrateEdge(L[0], L[1]);
    vsum += integrateEdge(L[1], L[2]);
    vsum += integrateEdge(L[2], L[3]);
    vsum += integrateEdge(L[3], L[0]);

    const float len = length(vsum);
    float z = vsum.z / max(len, 1e-8);
    if (behind) {
        z = -z;
    }

    vec2 uv = vec2(z * 0.5 + 0.5, len);
    uv = uv * LUT_SCALE + LUT_BIAS;
    const float scale = texture(LTC2, uv).w;

    float sum = len * scale;
    if (!behind && !twoSided) {
        sum = 0.0;
    }
    return vec3(sum);
}

void main()
{
    const vec3 albedo = ubo.material.rgb;
    const float roughness = clamp(ubo.material.w, 0.02, 1.0);
    const vec3 N = normalize(inNormal);
    const vec3 V = normalize(ubo.eye.xyz - inWorldPos);
    const float dotNV = clamp(dot(N, V), 0.0, 1.0);

    // (roughness, sqrt(1 - cos)) is the tabulated parameterisation.
    vec2 uv = vec2(roughness, sqrt(1.0 - dotNV));
    uv = uv * LUT_SCALE + LUT_BIAS;
    const vec4 t1 = texture(LTC1, uv);
    const vec4 t2 = texture(LTC2, uv);

    // LTC1 packs the inverse matrix as [x 0 z; 0 1 0; y 0 w].
    const mat3 Minv = mat3(vec3(t1.x, 0.0, t1.y), vec3(0.0, 1.0, 0.0), vec3(t1.z, 0.0, t1.w));

    const int lightCount = int(ubo.params.x + 0.5);
    const bool twoSided = ubo.params.w > 0.5;
    const bool diffuseOnly = ubo.params.z > 0.5;

    // The reference fakes a checker floor with a texture; a procedural 1 m
    // checker is equivalent and needs no asset.
    vec3 base = albedo;
    const vec2 cell = floor(inUV * 10.0);
    base *= 0.78 + 0.22 * mod(cell.x + cell.y, 2.0);

    // Dielectric F0 of the reference's grey floor.
    const vec3 mSpecular = vec3(0.23);

    vec3 result = vec3(0.0);
    for (int i = 0; i < lightCount; ++i) {
        const AreaLight light = lights[i];
        vec3 points[4] = vec3[4](light.p0.xyz, light.p1.xyz, light.p2.xyz, light.p3.xyz);
        const bool lightTwoSided = twoSided || light.params.y > 0.5;

        const vec3 diffuse = evaluate(N, V, inWorldPos, mat3(1.0), points, lightTwoSided);
        vec3 specular = vec3(0.0);
        if (!diffuseOnly) {
            specular = evaluate(N, V, inWorldPos, Minv, points, lightTwoSided);
            // t2.x = F90 scale, t2.y = Smith geometry term, both from LTC2.
            specular *= mSpecular * t2.x + (1.0 - mSpecular) * t2.y;
        }

        result += light.color.rgb * light.params.x * ubo.params.y * (specular + base * diffuse);
    }

    // No manual gamma here: the swapchain is B8G8R8A8_SRGB, so the hardware
    // applies the sRGB transfer function on write.
    outColor = vec4(result, 1.0);
}
