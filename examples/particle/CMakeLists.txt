vulkan_compile_shader(INFILE shader/particle.comp OUTFILE shader/particle_comp.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/particle.vert OUTFILE shader/particle_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/particle.frag OUTFILE shader/particle_frag.hpp NAMESPACE "shader")
add_render_example(particle test5.cpp
    shader/particle_comp.hpp shader/particle_vert.hpp shader/particle_frag.hpp)
