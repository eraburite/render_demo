#version 450
// Vertex stage of the model_loading port.
//
// Mirrors LearnOpenGL's 1.model_loading.vs (position/texcoord in, texcoord out)
// with the three separate mat4 uniforms folded into one std140 block.  The
// tutorial also declares aNormal and never reads it, so the framework pipeline
// only asks for the two attributes that reach the fragment stage.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(set = 1, binding = 0) uniform ModelViewBlock
{
    mat4 view;
    mat4 proj;
    mat4 model;
} transform;

layout(location = 0) out vec2 outUV;

void main()
{
    outUV = inUV;
    // Projection already carries the Vulkan Y flip (RenderCommon::
    // MakeCameraMatrix), so no extra sign juggling here.
    gl_Position = transform.proj * transform.view * transform.model * vec4(inPos, 1.0);
}
