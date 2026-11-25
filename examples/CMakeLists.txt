# Helper: define one render example with generated shader headers as deps.
function(add_render_example target main_src)
    add_executable(${target} ${main_src} ${ARGN})
    target_link_libraries(${target} PRIVATE example_common myEngine globalConfig)
    add_dependencies(${target} myEngine)
endfunction()

add_subdirectory(simplest)
add_subdirectory(multi-lighting)
add_subdirectory(offscreen)
add_subdirectory(gbuffer)
add_subdirectory(particle)
add_subdirectory(raw2rgb)
add_subdirectory(geometryshader)
# Extended examples (migrated from Sascha Willems' Vulkan Examples).  The
# conditional guard keeps an incomplete checkout configurable while the example
# directories are being populated; add the directory once its CMakeLists exists.
foreach(example tessellation deferred terraintessellation gltfloading gltfscenerendering multithreading)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${example}/CMakeLists.txt")
        add_subdirectory(${example})
    endif()
endforeach()
