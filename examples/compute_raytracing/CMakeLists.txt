vulkan_compile_shader(INFILE shader/raymarch.comp OUTFILE shader/raymarch_comp.hpp NAMESPACE "shader")

add_render_example(compute_raytracing test48.cpp
    shader/raymarch_comp.hpp)
