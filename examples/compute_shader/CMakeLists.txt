vulkan_compile_shader(INFILE shader/compute.comp OUTFILE shader/compute_comp.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.vert OUTFILE shader/screen_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.frag OUTFILE shader/screen_frag.hpp NAMESPACE "shader")

add_render_example(compute_shader test33.cpp
    shader/compute_comp.hpp shader/screen_vert.hpp shader/screen_frag.hpp)
