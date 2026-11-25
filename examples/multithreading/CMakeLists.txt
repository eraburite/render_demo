vulkan_compile_shader(INFILE shader/cube.vert OUTFILE shader/cube_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/cube.frag OUTFILE shader/cube_frag.hpp NAMESPACE "shader")

add_render_example(multithreading test12.cpp
    shader/cube_vert.hpp shader/cube_frag.hpp)
