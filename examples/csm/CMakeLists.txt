vulkan_compile_shader(INFILE shader/csm.vert OUTFILE shader/csm_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/csm.frag OUTFILE shader/csm_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.vert OUTFILE shader/depth_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.geom OUTFILE shader/depth_geom.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.frag OUTFILE shader/depth_frag.hpp NAMESPACE "shader")

add_render_example(csm test26.cpp
    shader/csm_vert.hpp shader/csm_frag.hpp shader/depth_vert.hpp shader/depth_geom.hpp shader/depth_frag.hpp)
