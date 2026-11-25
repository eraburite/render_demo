vulkan_compile_shader(INFILE shader/mesh.vert OUTFILE shader/mesh_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/mesh.frag OUTFILE shader/mesh_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/normaldebug.geom OUTFILE shader/normaldebug_geom.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/normaldebug.frag OUTFILE shader/normaldebug_frag.hpp NAMESPACE "shader")

add_render_example(geometryshader test7.cpp
    shader/mesh_vert.hpp shader/mesh_frag.hpp shader/normaldebug_geom.hpp shader/normaldebug_frag.hpp)
