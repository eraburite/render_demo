#version 450

layout (triangles) in;
layout (line_strip, max_vertices = 6) out;

layout (set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProj;
} ubo;

layout (location = 0) in vec3 inNormal[];

layout (location = 0) out vec3 outColor;

void main()
{
    const float normalLength = 0.05;
    for (int i = 0; i < gl_in.length(); i++) {
        vec3 pos = gl_in[i].gl_Position.xyz;
        vec3 normal = normalize(inNormal[i]);

        // Start point: red, at the (clip-space) vertex position.
        gl_Position = vec4(pos, 1.0);
        outColor = vec3(1.0, 0.0, 0.0);
        EmitVertex();

        // End point: blue, offset along the normal in clip space.
        gl_Position = vec4(pos + normal * normalLength, 1.0);
        outColor = vec3(0.0, 0.0, 1.0);
        EmitVertex();

        EndPrimitive();
    }
}
