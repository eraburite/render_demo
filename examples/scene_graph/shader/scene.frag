#version 450

// Single directional light plus ambient.  The point of this example is the
// scene graph and the culling, so the shading stays deliberately dull -- but it
// has to react to the normal, otherwise a sphere and a disc look the same and a
// culling bug that drops half the scene is hard to see.

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inWorldPos;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;

layout(location = 0) out vec4 fragColor;

void main()
{
    const vec3 albedo = texture(diffuseMap, inUV).rgb;
    const vec3 normal = normalize(inNormal);
    const vec3 lightDirection = normalize(vec3(0.35, 0.8, 0.45));
    const float diffuse = max(dot(normal, lightDirection), 0.0);
    // 0.25 ambient keeps the side facing away from the light readable, which is
    // what makes a culled entity's absence obvious in a dump.
    const vec3 color = albedo * (0.25 + 0.75 * diffuse);
    fragColor = vec4(color, 1.0);
}
