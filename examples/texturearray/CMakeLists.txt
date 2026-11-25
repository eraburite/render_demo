vulkan_compile_shader(INFILE shader/instancing.vert OUTFILE shader/instancing_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/instancing.frag OUTFILE shader/instancing_frag.hpp NAMESPACE "shader")

add_render_example(texturearray test16.cpp
    shader/instancing_vert.hpp shader/instancing_frag.hpp)
