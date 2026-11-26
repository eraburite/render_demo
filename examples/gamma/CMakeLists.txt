vulkan_compile_shader(INFILE shader/gamma.vert OUTFILE shader/gamma_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/gamma.frag OUTFILE shader/gamma_frag.hpp NAMESPACE "shader")

add_render_example(gamma test29.cpp shader/gamma_vert.hpp shader/gamma_frag.hpp)
