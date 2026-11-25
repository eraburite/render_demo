vulkan_compile_shader(INFILE shader/scene.vert OUTFILE shader/scene_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/scene.frag OUTFILE shader/scene_frag.hpp NAMESPACE "shader")

add_render_example(gltfscenerendering test13.cpp
    shader/scene_vert.hpp shader/scene_frag.hpp)
