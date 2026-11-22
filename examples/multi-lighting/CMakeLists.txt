vulkan_compile_shader(INFILE shader/lighting.vert OUTFILE shader/lighting_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/lighting.frag OUTFILE shader/lighting_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/light_cube.vert OUTFILE shader/light_cube_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/light_cube.frag OUTFILE shader/light_cube_frag.hpp NAMESPACE "shader")
add_render_example(multi-lighting test2.cpp
    shader/lighting_vert.hpp shader/lighting_frag.hpp
    shader/light_cube_vert.hpp shader/light_cube_frag.hpp)
