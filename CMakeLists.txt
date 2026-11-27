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

# ---------------------------------------------------------------------------
# Layering (see DESIGN.md 30).  Each layer only depends on the ones above it:
#   globalConfig         include dirs / link libs shared by everything
#   myEngine (src/)      core renderer, no dependency on anything below
#   frameworkExtensions  model / texture / debug-dump capabilities
#   example_common       example-side glue (window / camera / configs / font)
#   examples/*           41 demo executables
# The only link-time third-party SDK dependency is FreeType, kept in
# example_common behind RENDER_DEMO_HAS_FREETYPE; the extension layer builds
# from a fresh clone with no external checkout.
# ---------------------------------------------------------------------------

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

# myEngine: the core static library.
file(GLOB ENGINE_SRC CONFIGURE_DEPENDS "src/*.cpp")
add_library(myEngine STATIC ${ENGINE_SRC})
target_link_libraries(myEngine PUBLIC globalConfig)

# frameworkExtensions (extensions/): model loaders, texture loaders, PPM dump.
# Defined after myEngine because it uses the core's ImageReadback / typedefs.
add_subdirectory(extensions)

# example_common: example-side glue compiled once (GLFWwindow / FontAtlas).
# CONFIGURE_DEPENDS re-globs on every build, so adding a .cpp here does not need
# a manual re-configure.  Linking frameworkExtensions PUBLIC-ly gives all 41
# examples the extension headers and symbols without touching each example
# target (add_render_example still links example_common + myEngine + globalConfig).
file(GLOB EXAMPLE_COMMON_SRC CONFIGURE_DEPENDS "examples/common/*.cpp")
add_library(example_common STATIC ${EXAMPLE_COMMON_SRC})
target_link_libraries(example_common PUBLIC globalConfig frameworkExtensions)

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

add_subdirectory(examples)
