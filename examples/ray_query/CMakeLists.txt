# Ray query lives in an ordinary compute shader, so the stage-identifying
# extension cannot imply the target environment; it is opted in explicitly.
vulkan_compile_shader(INFILE shader/scene.comp OUTFILE shader/scene_comp.hpp NAMESPACE "shader"
    TARGET_ENV "vulkan1.2")

add_render_example(ray_query test47.cpp
    shader/scene_comp.hpp)
