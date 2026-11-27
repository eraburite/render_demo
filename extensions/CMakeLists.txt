# ---------------------------------------------------------------------------
# frameworkExtensions: optional capabilities layered on top of myEngine.
#
# Directory layout (a sibling of src/, see DESIGN.md 30):
#   model/    - loader-neutral model data + registry + built-in format loaders
#   texture/  - stb_image / KTX 1.0 decoders
#   debug/    - PPM dump built on the core's ImageReadback
#
# Third-party dependency policy (DESIGN.md 30.2):
#   * zero third-party: ModelAsset / ModelLoader / PostProcess / Builder /
#     BinaryReader / FormatsCommon, PMX / PMD / XFile / VMD, SkeletonAnimator,
#     KtxLoader, ImageDump -- only Vulkan + GLM (already core deps) + stdlib.
#   * header-only, vendored in external/ (shipped with the repo, always on):
#     tinygltf + stb_image (GltfModelLoader), tiny_obj_loader (ObjModelLoader),
#     stb_image (TextureLoader).
#   * assimp: vendored *headers* only; the importer DLL is bound at run time via
#     GetProcAddress, so there is no link-time import and no SDK is needed to
#     build.  A missing DLL only makes that one loader report "unavailable".
#   * FreeType is deliberately NOT here: it needs a link-time .lib, so the glyph
#     atlas stays in examples/common behind RENDER_DEMO_HAS_FREETYPE.
#
# One static library (not three) is intentional: GltfModelLoader.cpp is the
# single TU that defines STB_IMAGE_IMPLEMENTATION / TINYGLTF_IMPLEMENTATION for
# TextureLoader.cpp and ObjModelLoader.cpp, and BuiltinModelLoaders.cpp is the
# explicit link anchor that pulls every loader object into the registry.
# Splitting the library would break both mechanisms.
# ---------------------------------------------------------------------------

file(GLOB FRAMEWORK_EXT_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/model/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/debug/*.cpp")
add_library(frameworkExtensions STATIC ${FRAMEWORK_EXT_SRC})

# Keep the short, unqualified #include "ModelLoader.h" style used by all 41
# examples: every capability directory is a public include root.
target_include_directories(frameworkExtensions PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/model
    ${CMAKE_CURRENT_SOURCE_DIR}/texture
    ${CMAKE_CURRENT_SOURCE_DIR}/debug
)
target_link_libraries(frameworkExtensions PUBLIC myEngine globalConfig)

# ---- assimp runtime (vendored headers + run-time DLL binding) --------------
# The headers under external/assimp are part of the repository (see .gitignore),
# so the API is known at compile time without any external checkout.  No assimp
# symbol is referenced at link time: every entry point is fetched with
# GetProcAddress inside AssimpModelLoader.cpp.  This block only tells the loader
# where to look for the DLL; a build with no discoverable DLL still compiles and
# the loader reports why it is unavailable instead of failing at process start.
target_include_directories(frameworkExtensions PUBLIC
    "${CMAKE_SOURCE_DIR}/external/assimp/include")

set(RENDER_DEMO_ASSIMP_DIR "" CACHE PATH "Directory containing dlls/assimp-vc140-mt.dll")
if(NOT RENDER_DEMO_ASSIMP_DIR AND DEFINED ENV{ASSIMP_DIR})
    set(RENDER_DEMO_ASSIMP_DIR "$ENV{ASSIMP_DIR}")
endif()
if(NOT RENDER_DEMO_ASSIMP_DIR)
    foreach(candidate "D:/LearnOpenGL" "C:/LearnOpenGL" "$ENV{USERPROFILE}/LearnOpenGL")
        if(EXISTS "${candidate}/dlls/assimp-vc140-mt.dll")
            set(RENDER_DEMO_ASSIMP_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(RENDER_DEMO_ASSIMP_DIR)
    set(RENDER_DEMO_ASSIMP_DLL "${RENDER_DEMO_ASSIMP_DIR}/dlls/assimp-vc140-mt.dll")
    target_compile_definitions(frameworkExtensions PRIVATE
        RENDER_DEMO_ASSIMP_DLL_HINT="${RENDER_DEMO_ASSIMP_DLL}")
    message(STATUS "assimp runtime hint: ${RENDER_DEMO_ASSIMP_DLL}")
else()
    message(STATUS "assimp runtime hint: none found (set RENDER_DEMO_ASSIMP_DIR to enable .dae/.fbx loading)")
endif()
