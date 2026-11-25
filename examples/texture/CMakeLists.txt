vulkan_compile_shader(INFILE shader/texture.vert OUTFILE shader/texture_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/texture.frag OUTFILE shader/texture_frag.hpp NAMESPACE "shader")

add_render_example(texture test14.cpp
    shader/texture_vert.hpp shader/texture_frag.hpp)
