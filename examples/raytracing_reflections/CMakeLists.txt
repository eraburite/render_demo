vulkan_compile_shader(INFILE shader/raygen.rgen OUTFILE shader/raygen_rgen.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/miss.rmiss OUTFILE shader/miss_rmiss.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/closesthit.rchit OUTFILE shader/closesthit_rchit.hpp NAMESPACE "shader")

add_render_example(raytracing_reflections test46.cpp
    shader/raygen_rgen.hpp
    shader/miss_rmiss.hpp
    shader/closesthit_rchit.hpp)
