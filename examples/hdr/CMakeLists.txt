vulkan_compile_shader(INFILE shader/scene.vert OUTFILE shader/scene_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/scene.frag OUTFILE shader/scene_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.vert OUTFILE shader/screen_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/tonemap.frag OUTFILE shader/tonemap_frag.hpp NAMESPACE "shader")

add_render_example(hdr test27.cpp
    shader/scene_vert.hpp shader/scene_frag.hpp
    shader/screen_vert.hpp shader/tonemap_frag.hpp)
