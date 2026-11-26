vulkan_compile_shader(INFILE shader/scene.vert OUTFILE shader/scene_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/scene.frag OUTFILE shader/scene_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.vert OUTFILE shader/depth_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/depth.frag OUTFILE shader/depth_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/quad.vert OUTFILE shader/quad_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/quad.frag OUTFILE shader/quad_frag.hpp NAMESPACE "shader")

add_render_example(shadow_mapping test30.cpp
    shader/scene_vert.hpp shader/scene_frag.hpp shader/depth_vert.hpp shader/depth_frag.hpp
    shader/quad_vert.hpp shader/quad_frag.hpp)
