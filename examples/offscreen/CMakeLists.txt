vulkan_compile_shader(INFILE shader/phong.vert OUTFILE shader/phong_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/phong.frag OUTFILE shader/phong_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/quad.vert OUTFILE shader/quad_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/quad.frag OUTFILE shader/quad_frag.hpp NAMESPACE "shader")
add_render_example(offscreen test3.cpp
    shader/phong_vert.hpp shader/phong_frag.hpp
    shader/quad_vert.hpp shader/quad_frag.hpp)
