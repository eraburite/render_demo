vulkan_compile_shader(INFILE shader/mesh.vert OUTFILE shader/mesh_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/mesh.frag OUTFILE shader/mesh_frag.hpp NAMESPACE "shader")

add_render_example(gltfloading test10.cpp
    shader/mesh_vert.hpp shader/mesh_frag.hpp)
