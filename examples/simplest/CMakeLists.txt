vulkan_compile_shader(INFILE shader/depth_buffer.vert OUTFILE shader/depth_buffer_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth_buffer.frag OUTFILE shader/depth_buffer_frag.hpp NAMESPACE "shader")
add_render_example(simplest test1.cpp shader/depth_buffer_vert.hpp shader/depth_buffer_frag.hpp)
