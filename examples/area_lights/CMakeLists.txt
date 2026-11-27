vulkan_compile_shader(INFILE shader/plane.vert OUTFILE shader/plane_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/plane.frag OUTFILE shader/plane_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/light.vert OUTFILE shader/light_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/light.frag OUTFILE shader/light_frag.hpp NAMESPACE "shader")

add_render_example(area_lights test35.cpp
    shader/plane_vert.hpp shader/plane_frag.hpp
    shader/light_vert.hpp shader/light_frag.hpp)
