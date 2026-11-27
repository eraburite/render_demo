vulkan_compile_shader(INFILE shader/model.vert OUTFILE shader/model_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/model.frag OUTFILE shader/model_frag.hpp NAMESPACE "shader")

add_render_example(model_formats test41.cpp
    shader/model_vert.hpp shader/model_frag.hpp)
