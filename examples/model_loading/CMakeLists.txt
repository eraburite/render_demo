vulkan_compile_shader(INFILE shader/model.vert OUTFILE shader/model_vert.hpp NAMESPACE "shader")
vulkan_compile_shader(INFILE shader/model.frag OUTFILE shader/model_frag.hpp NAMESPACE "shader")

add_render_example(model_loading test21.cpp
    shader/model_vert.hpp shader/model_frag.hpp)

# AssimpModelLoader resolves the importer by bare file name first, so put a copy
# next to the executable when CMake found an SDK.  Nothing is linked against it:
# a missing DLL only makes the assimp formats unavailable at run time.
if(RENDER_DEMO_ASSIMP_DLL)
    add_custom_command(TARGET model_loading POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RENDER_DEMO_ASSIMP_DLL}" "$<TARGET_FILE_DIR:model_loading>/assimp-vc140-mt.dll"
        COMMENT "Copying the assimp runtime next to model_loading")
endif()
