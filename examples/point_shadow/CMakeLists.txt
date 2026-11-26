vulkan_compile_shader(INFILE shader/depth.vert OUTFILE shader/depth_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.geom OUTFILE shader/depth_geom.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.frag OUTFILE shader/depth_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/point_shadow.vert OUTFILE shader/point_shadow_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/point_shadow.frag OUTFILE shader/point_shadow_frag.hpp NAMESPACE "shader")

add_render_example(point_shadow test31.cpp
    shader/depth_vert.hpp shader/depth_geom.hpp shader/depth_frag.hpp
    shader/point_shadow_vert.hpp shader/point_shadow_frag.hpp)
