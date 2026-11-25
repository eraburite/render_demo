#version 450
// Fragment stage of the model_loading port: exactly what the tutorial's
// 1.model_loading.fs does, one sampled map written straight to the framebuffer.
//
// The map is uploaded as R8G8B8A8_SRGB and the swapchain is sRGB too, so the
// sampler's decode and the attachment's encode cancel out and the image comes out
// looking like the source file -- the same result OpenGL gives when it hands the
// JPEG bytes to a plain GL_RGB texture.
layout(set = 0, binding = 0) uniform sampler2D diffuseMap;

layout(location = 0) in vec2 outUV;

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(diffuseMap, outUV);
}
