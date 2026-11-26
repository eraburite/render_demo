#version 450

// SSAO pass, matching 9.ssao.fs.  64 hemisphere samples are rotated into view
// space per fragment by a TBN built from the surface normal and a 4x4 tiling
// noise texture.  Each sample is projected to screen space and compared against
// the depth stored in gPosition.  The reference's exact parameters are used:
// radius 0.5, bias 0.025, smoothstep range check, occlusion = 1 - sum / 64.
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform SsaoBlock
{
    mat4 projection;
    vec4 noiseScale; // xy = width / 4, height / 4
    vec4 params;     // x = radius, y = bias, z = kernelSize, w = ssao enable
} ubo;

layout(set = 1, binding = 0) uniform sampler2D gPosition;
layout(set = 1, binding = 1) uniform sampler2D gNormal;
layout(set = 1, binding = 2) uniform sampler2D texNoise;

// std430 vec3 has a 16-byte stride; the CPU side pads each sample accordingly.
layout(std430, set = 2, binding = 0) readonly buffer SsaoKernel
{
    vec3 samples[64];
} kernel;

void main()
{
    const vec4 positionSample = texture(gPosition, inUV);
    if (positionSample.w < 0.5) {
        outColor = vec4(1.0); // no geometry -> fully unoccluded
        return;
    }
    const vec3 fragPos = positionSample.xyz;
    const vec3 normal = normalize(texture(gNormal, inUV).xyz);
    // Noise is stored as [0,1] RGBA8; remap to [-1,1] and tile every 4 pixels.
    const vec3 randomVec = normalize(texture(texNoise, inUV * ubo.noiseScale.xy).xyz * 2.0 - 1.0);
    const vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    const vec3 bitangent = cross(normal, tangent);
    const mat3 TBN = mat3(tangent, bitangent, normal);

    const int kernelSize = int(ubo.params.z + 0.5);
    const float radius = ubo.params.x;
    const float bias = ubo.params.y;
    float occlusion = 0.0;
    for (int i = 0; i < kernelSize; ++i) {
        vec3 samplePos = TBN * kernel.samples[i];
        samplePos = fragPos + samplePos * radius;
        vec4 offset = ubo.projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;
        const float sampleDepth = texture(gPosition, offset.xy).z;
        // Reject samples beyond the radius so nearby surfaces do not darken
        // unrelated pixels (the reference uses the same smoothstep falloff).
        const float rangeCheck = smoothstep(0.0, 1.0, radius / max(abs(fragPos.z - sampleDepth), 1e-4));
        occlusion += (sampleDepth >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    const float ao = 1.0 - occlusion / float(kernelSize);
    outColor = vec4(vec3(ao), 1.0);
}
