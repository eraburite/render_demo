vulkan_compile_shader(INFILE shader/terrain.vert OUTFILE shader/terrain_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain.tesc OUTFILE shader/terrain_tesc.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain.tese OUTFILE shader/terrain_tese.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain.frag OUTFILE shader/terrain_frag.hpp NAMESPACE "shader")

add_render_example(terraintessellation test11.cpp
    shader/terrain_vert.hpp shader/terrain_tesc.hpp shader/terrain_tese.hpp shader/terrain_frag.hpp)
