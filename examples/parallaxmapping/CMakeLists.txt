vulkan_compile_shader(INFILE shader/parallax.vert OUTFILE shader/parallax_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/parallax.frag OUTFILE shader/parallax_frag.hpp NAMESPACE "shader")

add_render_example(parallaxmapping test17.cpp
    shader/parallax_vert.hpp shader/parallax_frag.hpp)
