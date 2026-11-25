#version 450
// Saturated magenta is deliberately far from the toon blue and the dark clear
// colour, so a captured frame can be counted by a simple RGB threshold.

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(1.0, 0.12, 0.72, 1.0);
}
