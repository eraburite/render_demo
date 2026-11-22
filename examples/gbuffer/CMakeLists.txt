vulkan_compile_shader(INFILE shader/mrt.vert OUTFILE shader/mrt_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/mrt.frag OUTFILE shader/mrt_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/deferred.vert OUTFILE shader/deferred_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/deferred.frag OUTFILE shader/deferred_frag.hpp NAMESPACE "shader")
add_render_example(gbuffer test4.cpp
    shader/mrt_vert.hpp shader/mrt_frag.hpp
    shader/deferred_vert.hpp shader/deferred_frag.hpp)
