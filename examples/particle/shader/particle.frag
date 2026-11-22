#version 450
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 0) out vec4 outColor;
void main() {
    float d = length(fragUV);
    if (d > 1.0) discard;
    outColor = vec4(fragColor.rgb, fragColor.a * (1.0 - d));
}
