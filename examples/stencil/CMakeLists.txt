vulkan_compile_shader(INFILE shader/toon.vert OUTFILE shader/toon_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/toon.frag OUTFILE shader/toon_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/outline.vert OUTFILE shader/outline_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/outline.frag OUTFILE shader/outline_frag.hpp NAMESPACE "shader")

add_render_example(stencil test20.cpp
    shader/toon_vert.hpp shader/toon_frag.hpp
    shader/outline_vert.hpp shader/outline_frag.hpp)
