vulkan_compile_shader(INFILE shader/scene.vert OUTFILE shader/scene_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/scene.frag OUTFILE shader/scene_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/blur_h.frag OUTFILE shader/blur_h_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/blur_v.frag OUTFILE shader/blur_v_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.vert OUTFILE shader/screen_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/composite.frag OUTFILE shader/composite_frag.hpp NAMESPACE "shader")

add_render_example(bloom test28.cpp
    shader/scene_vert.hpp shader/scene_frag.hpp
    shader/blur_h_frag.hpp shader/blur_v_frag.hpp
    shader/screen_vert.hpp shader/composite_frag.hpp)
