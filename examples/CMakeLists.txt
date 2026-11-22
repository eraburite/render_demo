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
