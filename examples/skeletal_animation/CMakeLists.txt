vulkan_compile_shader(INFILE shader/skeletal.vert OUTFILE shader/skeletal_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/skeletal.frag OUTFILE shader/skeletal_frag.hpp NAMESPACE "shader")

add_render_example(skeletal_animation test23.cpp
    shader/skeletal_vert.hpp shader/skeletal_frag.hpp)

# Same runtime-only dependency as model_loading: the loader binds assimp with
# LoadLibraryW, and this example is the one that actually needs it.
if(RENDER_DEMO_ASSIMP_DLL)
    add_custom_command(TARGET skeletal_animation POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RENDER_DEMO_ASSIMP_DLL}" "$<TARGET_FILE_DIR:skeletal_animation>"
        COMMENT "Copying assimp runtime next to skeletal_animation")
endif()
