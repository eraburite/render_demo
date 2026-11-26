#version 450

// One fragment shader for both displacement paths: the CPU matrix and the GPU
// tessellator write exactly these three varyings, so the only difference
// between the two modes is where the geometry came from.  Anything that looks
// different in a dump is therefore a geometry difference, not a shading one.
//
// --grayscale reproduces the tutorial's shading (a bare ramp of the height),
// which is the cheapest way to check the displacement without lighting hiding
// a mistake in it.

layout (set = 1, binding = 0) uniform TerrainUBO {
    mat4 viewProj;
    mat4 view;
    vec4 eye;     // xyz: camera position in world space
    vec4 height;  // x: height span, y: height shift, z: grayscale flag
    vec4 terrain; // x: world width, y: world depth, z: one texel in u, w: one texel in v
    vec4 tess;    // x: min level, y: max level, z: min distance, w: max distance
} ubo;

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inWorldPos;
layout (location = 2) in float inHeight;

layout (location = 0) out vec4 outFragColor;

// Low sun from the left: with a light behind the camera every slope facing the
// viewer is lit equally and the relief flattens out.
const vec3 kSunDirection = vec3(-0.70, 0.45, 0.25);
// Exactly the render pass' clear colour, so the far edge of the terrain melts
// into the background instead of ending on a visible line.
const vec3 kFogColor = vec3(0.05, 0.06, 0.09);

void main()
{
    // Height normalised to [0, 1] over the heightmap's full range.  Both paths
    // feed the same span/shift, so a given altitude always maps to the same
    // value -- the tutorial's CPU variant divided by 32 and its GPU variant by
    // 64 for the same height range, which is why one of them blew out to white.
    float altitude = clamp((inHeight + ubo.height.y) / ubo.height.x, 0.0, 1.0);

    if (ubo.height.z > 0.5) {
        outFragColor = vec4(vec3(altitude), 1.0);
        return;
    }

    vec3 N = normalize(inNormal);
    vec3 L = normalize(kSunDirection);
    vec3 V = normalize(ubo.eye.xyz - inWorldPos);
    vec3 R = reflect(-L, N);

    // Grass in the lowlands, bare rock where the surface is steep, snow on the
    // high flats.
    float slope = 1.0 - clamp(N.y, 0.0, 1.0);
    vec3 albedo = mix(vec3(0.20, 0.24, 0.15), vec3(0.36, 0.32, 0.24), smoothstep(0.05, 0.55, altitude));
    albedo = mix(albedo, vec3(0.42, 0.40, 0.37), smoothstep(0.05, 0.28, slope));
    albedo = mix(albedo, vec3(0.92, 0.94, 0.98),
                 smoothstep(0.68, 0.84, altitude) * (1.0 - smoothstep(0.10, 0.26, slope)));

    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(R, V), 0.0), 32.0) * 0.10;
    vec3 color = (0.22 + 0.90 * diffuse) * albedo + specular;

    // Atmospheric depth only: the camera is framed so the plate's boundary stays
    // outside the frame, so this does not have to hide anything.
    float distance = length(ubo.eye.xyz - inWorldPos);
    float fog = clamp((distance - 0.40 * ubo.terrain.x) / (0.70 * ubo.terrain.x), 0.0, 1.0);
    color = mix(color, kFogColor, fog);

    outFragColor = vec4(color, 1.0);
}
