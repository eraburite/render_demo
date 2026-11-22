cmake_minimum_required(VERSION 3.22)
project(render_demo LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
if(Vulkan_FOUND AND NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIRS}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(vulkan_shader_compiler)

# Generated configuration: absolute path to the repository root.
set(PROJECT_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
configure_file(configuration/root_directory.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/root_directory.h @ONLY)

# globalConfig: the single interface target providing include dirs and link libs.
add_library(globalConfig INTERFACE)
target_include_directories(globalConfig INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/examples/common>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/glm/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)
target_link_directories(globalConfig INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/lib)
target_link_libraries(globalConfig INTERFACE glfw3 Vulkan::Vulkan)

# example_common: compiled once (GLFWwindow / TextureLoader / ImageDump / glm.cpp)
# CONFIGURE_DEPENDS re-globs on every build, so adding a .cpp here does not need
# a manual re-configure.
file(GLOB EXAMPLE_COMMON_SRC CONFIGURE_DEPENDS "examples/common/*.cpp")
add_library(example_common STATIC ${EXAMPLE_COMMON_SRC})
target_link_libraries(example_common PUBLIC globalConfig)

# myEngine: the core static library.
file(GLOB ENGINE_SRC CONFIGURE_DEPENDS "src/*.cpp")
add_library(myEngine STATIC ${ENGINE_SRC})
target_link_libraries(myEngine PUBLIC globalConfig)

add_subdirectory(examples)
