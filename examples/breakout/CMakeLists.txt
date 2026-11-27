vulkan_compile_shader(INFILE shader/sprite.vert OUTFILE shader/sprite_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/sprite.frag OUTFILE shader/sprite_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/particle.vert OUTFILE shader/particle_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/particle.frag OUTFILE shader/particle_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/post.vert OUTFILE shader/post_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/post.frag OUTFILE shader/post_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/text2d.vert OUTFILE shader/text2d_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/text2d.frag OUTFILE shader/text2d_frag.hpp NAMESPACE "shader")

add_render_example(breakout test37.cpp
    shader/sprite_vert.hpp shader/sprite_frag.hpp
    shader/particle_vert.hpp shader/particle_frag.hpp
    shader/post_vert.hpp shader/post_frag.hpp
    shader/text2d_vert.hpp shader/text2d_frag.hpp)
