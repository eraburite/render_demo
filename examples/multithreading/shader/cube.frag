#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outFragColor;

void main()
{
    // Culled instances carry a near-zero matrix; their triangles are degenerate
    // and never reach the fragment stage.  Guard the normalise anyway so a
    // numerical edge case cannot produce NaN colours.
    vec3 n = inNormal;
    n = dot(n, n) > 1e-8 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    vec3 l = normalize(vec3(0.5, 0.8, 1.0));
    float diffuse = max(dot(n, l), 0.0);
    outFragColor = vec4(inColor.rgb * (0.25 + 0.75 * diffuse), 1.0);
}
