vulkan_compile_shader(INFILE shader/object.vert OUTFILE shader/object_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/solid.frag OUTFILE shader/solid_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/transparent.frag OUTFILE shader/transparent_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/screen.vert OUTFILE shader/screen_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/composite.frag OUTFILE shader/composite_frag.hpp NAMESPACE "shader")

add_render_example(oit test22.cpp
    shader/object_vert.hpp shader/solid_frag.hpp shader/transparent_frag.hpp
    shader/screen_vert.hpp shader/composite_frag.hpp)
