#version 450
// Three-band toon shading.  The UBO is vertex-stage only, so the light
// direction is a constant here; keeping the fragment shader free of the
// descriptor avoids a stage-visibility VUID.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 lightDir = normalize(vec3(0.45, 0.8, 0.55));
    float diffuse = max(dot(normalize(inNormal), lightDir), 0.0);
    float band = diffuse > 0.72 ? 1.0 : (diffuse > 0.40 ? 0.68 : (diffuse > 0.16 ? 0.42 : 0.24));
    vec3 base = inColor * vec3(0.30, 0.72, 0.95);
    outColor = vec4(base * band + vec3(0.05, 0.07, 0.09), 1.0);
}
