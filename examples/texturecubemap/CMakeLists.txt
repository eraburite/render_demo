vulkan_compile_shader(INFILE shader/skybox.vert OUTFILE shader/skybox_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/skybox.frag OUTFILE shader/skybox_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/reflect.vert OUTFILE shader/reflect_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/reflect.frag OUTFILE shader/reflect_frag.hpp NAMESPACE "shader")

add_render_example(texturecubemap test15.cpp
    shader/skybox_vert.hpp shader/skybox_frag.hpp
    shader/reflect_vert.hpp shader/reflect_frag.hpp)
