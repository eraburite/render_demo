vulkan_compile_shader(INFILE shader/text.vert OUTFILE shader/text_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/text.frag OUTFILE shader/text_frag.hpp NAMESPACE "shader")

add_render_example(text_rendering test36.cpp shader/text_vert.hpp shader/text_frag.hpp)
