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

# ---- assimp runtime for examples/common/AssimpModelLoader.cpp --------------
# The headers are vendored under external/assimp (so the build never depends on
# an external checkout), and the importer .dll is bound at run time by the
# loader.  All this block does is tell the loader where to look for it: a build
# with no discoverable DLL still compiles, and the loader then reports why it is
# unavailable instead of failing at process start like a link-time import would.
set(RENDER_DEMO_ASSIMP_DIR "" CACHE PATH "Directory containing dlls/assimp-vc140-mt.dll")
if(NOT RENDER_DEMO_ASSIMP_DIR AND DEFINED ENV{ASSIMP_DIR})
    set(RENDER_DEMO_ASSIMP_DIR "$ENV{ASSIMP_DIR}")
endif()
if(NOT RENDER_DEMO_ASSIMP_DIR)
    # The LearnOpenGL checkout this repo is developed next to ships a prebuilt
    # SDK; use it when present so a fresh clone builds a working demo here.
    foreach(candidate "D:/LearnOpenGL" "C:/LearnOpenGL" "$ENV{USERPROFILE}/LearnOpenGL")
        if(EXISTS "${candidate}/dlls/assimp-vc140-mt.dll")
            set(RENDER_DEMO_ASSIMP_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()

target_include_directories(example_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/assimp/include)
if(RENDER_DEMO_ASSIMP_DIR)
    set(RENDER_DEMO_ASSIMP_DLL "${RENDER_DEMO_ASSIMP_DIR}/dlls/assimp-vc140-mt.dll")
    target_compile_definitions(example_common PRIVATE RENDER_DEMO_ASSIMP_DLL_HINT="${RENDER_DEMO_ASSIMP_DLL}")
    message(STATUS "assimp runtime hint: ${RENDER_DEMO_ASSIMP_DLL}")
else()
    set(RENDER_DEMO_ASSIMP_DLL "")
    message(STATUS "assimp runtime hint: none found (set RENDER_DEMO_ASSIMP_DIR to enable .dae/.fbx loading)")
endif()

# ---- FreeType for examples/common/FontAtlas.cpp ----------------------------
# Unlike assimp this is a *link-time* dependency: the glyph rasteriser is called
# on the CPU once at load time, so there is nothing to defer.  The headers are
# vendored under external/freetype (same rule as assimp: the build must not
# depend on an external checkout), and only the static library is looked up on
# the machine.  When the library cannot be found the text example is skipped
# instead of failing the build -- everything else keeps working.
#
# The vendoring is not just tidiness: pointing the include path at LearnOpenGL's
# own includes/ directory also brings its GLFW/glm/stb_image copies into scope,
# and since a target's own include dirs are searched *before* the ones it
# inherits, that silently shadowed the headers this project builds with
# (GLFW_TRUE undeclared, stbi_load_16_from_memory missing).
target_include_directories(example_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/freetype/include)

set(RENDER_DEMO_FREETYPE_DIR "" CACHE PATH "Directory containing lib/freetype.lib")
if(NOT RENDER_DEMO_FREETYPE_DIR AND DEFINED ENV{FREETYPE_DIR})
    set(RENDER_DEMO_FREETYPE_DIR "$ENV{FREETYPE_DIR}")
endif()
if(NOT RENDER_DEMO_FREETYPE_DIR)
    foreach(candidate "D:/LearnOpenGL" "C:/LearnOpenGL" "$ENV{USERPROFILE}/LearnOpenGL")
        if(EXISTS "${candidate}/lib/freetype.lib")
            set(RENDER_DEMO_FREETYPE_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()

set(RENDER_DEMO_HAS_FREETYPE OFF)
if(RENDER_DEMO_FREETYPE_DIR AND EXISTS "${RENDER_DEMO_FREETYPE_DIR}/lib/freetype.lib")
    set(RENDER_DEMO_HAS_FREETYPE ON)
    target_link_libraries(example_common PUBLIC "${RENDER_DEMO_FREETYPE_DIR}/lib/freetype.lib")
    target_compile_definitions(example_common PUBLIC RENDER_DEMO_HAS_FREETYPE=1)
    message(STATUS "FreeType: ${RENDER_DEMO_FREETYPE_DIR}/lib/freetype.lib")
else()
    message(STATUS "FreeType: lib/freetype.lib not found (set RENDER_DEMO_FREETYPE_DIR to enable examples/text_rendering)")
endif()

# myEngine: the core static library.
file(GLOB ENGINE_SRC CONFIGURE_DEPENDS "src/*.cpp")
add_library(myEngine STATIC ${ENGINE_SRC})
target_link_libraries(myEngine PUBLIC globalConfig)

add_subdirectory(examples)
