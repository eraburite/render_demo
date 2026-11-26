vulkan_compile_shader(INFILE shader/gbuffer.vert OUTFILE shader/gbuffer_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/gbuffer.frag OUTFILE shader/gbuffer_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.vert OUTFILE shader/screen_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/ssao.frag OUTFILE shader/ssao_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/blur.frag OUTFILE shader/blur_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/composite.frag OUTFILE shader/composite_frag.hpp NAMESPACE "shader")

add_render_example(ssao test32.cpp
    shader/gbuffer_vert.hpp shader/gbuffer_frag.hpp shader/screen_vert.hpp
    shader/ssao_frag.hpp shader/blur_frag.hpp shader/composite_frag.hpp)
