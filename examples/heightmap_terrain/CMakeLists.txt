vulkan_compile_shader(INFILE shader/terrain_cpu.vert OUTFILE shader/terrain_cpu_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain_gpu.vert OUTFILE shader/terrain_gpu_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain_gpu.tesc OUTFILE shader/terrain_gpu_tesc.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain_gpu.tese OUTFILE shader/terrain_gpu_tese.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/terrain.frag OUTFILE shader/terrain_frag.hpp NAMESPACE "shader")

add_render_example(heightmap_terrain test25.cpp
    shader/terrain_cpu_vert.hpp shader/terrain_gpu_vert.hpp shader/terrain_gpu_tesc.hpp
    shader/terrain_gpu_tese.hpp shader/terrain_frag.hpp)
