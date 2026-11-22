vulkan_compile_shader(INFILE shader/rawunpack.comp OUTFILE shader/rawunpack_comp.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/raw2rgb_v2.comp OUTFILE shader/raw2rgb_v2_comp.hpp NAMESPACE "shader")
add_render_example(raw2rgb test6.cpp
    shader/rawunpack_comp.hpp shader/raw2rgb_v2_comp.hpp)
