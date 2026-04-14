cmake_minimum_required(VERSION 3.22)

project(render_demo)

find_package(Vulkan REQUIRED)

if(Vulkan_FOUND AND NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIRS}")
endif()


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(vulkan_shader_compiler)

# Compiling shader
# To add more shaders simply copy the vulkan_compile_shader command and replace it with your new shader
vulkan_compile_shader(
    INFILE shader/lighting.vert
    OUTFILE shader/lighting_vert.hpp
    NAMESPACE "shader")
vulkan_compile_shader(
    INFILE shader/lighting.frag
    OUTFILE shader/lighting_frag.hpp
    NAMESPACE "shader")


# Then add it to the library, so you can access it later in your code
add_library(shader INTERFACE 
    shader/lighting_vert.hpp
    shader/lighting_frag.hpp)

target_include_directories(shader INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

file(GLOB HEADFILES "src/*.h")
file(GLOB SRCFILES "src/*.cpp")

# Setting up main example code
add_executable(${CMAKE_PROJECT_NAME}
    external/glm/include/glm/detail/glm.cpp
    ${SRCFILES}
    ${HEADFILES}
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/glm/include)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/include)
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/lib)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE glfw3 shader Vulkan::Vulkan)
