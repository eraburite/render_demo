vulkan_compile_shader(INFILE shader/tess.vert OUTFILE shader/tess_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/tess.frag OUTFILE shader/tess_frag.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/passthrough.tesc OUTFILE shader/passthrough_tesc.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/passthrough.tese OUTFILE shader/passthrough_tese.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/pntriangles.tesc OUTFILE shader/pntriangles_tesc.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/pntriangles.tese OUTFILE shader/pntriangles_tese.hpp NAMESPACE "shader")

add_render_example(tessellation test8.cpp
    shader/tess_vert.hpp shader/tess_frag.hpp
    shader/passthrough_tesc.hpp shader/passthrough_tese.hpp
    shader/pntriangles_tesc.hpp shader/pntriangles_tese.hpp)
