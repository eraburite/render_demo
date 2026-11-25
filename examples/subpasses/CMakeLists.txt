vulkan_compile_shader(INFILE shader/gbuffer.vert OUTFILE shader/gbuffer_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/gbuffer.frag OUTFILE shader/gbuffer_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/composition.vert OUTFILE shader/composition_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/composition.frag OUTFILE shader/composition_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/transparent.vert OUTFILE shader/transparent_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/transparent.frag OUTFILE shader/transparent_frag.hpp NAMESPACE "shader")

add_render_example(subpasses test19.cpp
    shader/gbuffer_vert.hpp shader/gbuffer_frag.hpp
    shader/composition_vert.hpp shader/composition_frag.hpp
    shader/transparent_vert.hpp shader/transparent_frag.hpp)
