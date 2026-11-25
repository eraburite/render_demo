vulkan_compile_shader(INFILE shader/texture3d.vert OUTFILE shader/texture3d_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/texture3d.frag OUTFILE shader/texture3d_frag.hpp NAMESPACE "shader")

add_render_example(texture3d test18.cpp
    shader/texture3d_vert.hpp shader/texture3d_frag.hpp)
