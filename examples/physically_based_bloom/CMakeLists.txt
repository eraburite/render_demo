vulkan_compile_shader(INFILE shader/scene.vert OUTFILE shader/scene_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/scene.frag OUTFILE shader/scene_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.vert OUTFILE shader/screen_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/downsample.frag OUTFILE shader/downsample_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/upsample.frag OUTFILE shader/upsample_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/blur.frag OUTFILE shader/blur_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/composite.frag OUTFILE shader/composite_frag.hpp NAMESPACE "shader")

add_render_example(physically_based_bloom test34.cpp
    shader/scene_vert.hpp shader/scene_frag.hpp
    shader/screen_vert.hpp shader/downsample_frag.hpp shader/upsample_frag.hpp
    shader/blur_frag.hpp shader/composite_frag.hpp)
