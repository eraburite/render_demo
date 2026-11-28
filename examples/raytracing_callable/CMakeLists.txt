vulkan_compile_shader(INFILE shader/raygen.rgen OUTFILE shader/raygen_rgen.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/miss.rmiss OUTFILE shader/miss_rmiss.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/closesthit.rchit OUTFILE shader/closesthit_rchit.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/callable_red.rcall OUTFILE shader/callable_red_rcall.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/callable_green.rcall OUTFILE shader/callable_green_rcall.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/callable_blue.rcall OUTFILE shader/callable_blue_rcall.hpp NAMESPACE "shader")

add_render_example(raytracing_callable test44.cpp
    shader/raygen_rgen.hpp
    shader/miss_rmiss.hpp
    shader/closesthit_rchit.hpp
    shader/callable_red_rcall.hpp
    shader/callable_green_rcall.hpp
    shader/callable_blue_rcall.hpp)
