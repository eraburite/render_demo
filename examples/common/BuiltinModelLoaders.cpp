#include "ModelLoader.h"

#include "AssimpModelLoader.h"
#include "GltfModelLoader.h"
#include "ObjModelLoader.h"
#include "PmdModelLoader.h"
#include "PmxModelLoader.h"
#include "VmdAnimationLoader.h"
#include "XFileModelLoader.h"

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The single extension point for built-in formats.
//
// ModelLoader.cpp (the framework core) contains no concrete loader includes;
// this file is where they are named.  Adding a format is one row here plus the
// loader's own .h/.cpp -- no change to the registry, the pipeline, or any
// example.  This file is also the link anchor: because the registry ctor calls
// into it, the linker must pull in every loader object file referenced here,
// which pure static self-registration in a static library cannot guarantee on
// MSVC (an unreferenced TU would simply be dropped).
//
// priority resolves overlapping extension claims.  assimp advertises .obj and
// .gltf/.glb so it can be A/B-tested with --loader assimp, but the hand-written
// loaders must stay authoritative by default: a third-party library upgrade
// would otherwise silently change existing examples' output.
// ---------------------------------------------------------------------------

namespace {

template <typename LoaderT>
std::unique_ptr<IModelLoader> makeLoader()
{
    return std::make_unique<LoaderT>();
}

struct BuiltinRow {
    const char *name;
    std::vector<std::string> extensions;
    int priority;
    bool magicProbe;
    ModelLoaderFactory factory;
};

const std::vector<BuiltinRow> &builtinRows()
{
    static const std::vector<BuiltinRow> rows = {
        {"assimp", {"obj", "dae", "fbx", "3ds", "ply", "stl", "x3d", "md5mesh", "blend", "gltf", "glb"},
         0, false, &makeLoader<AssimpModelLoader>},
        {"gltf", {"gltf", "glb"}, 20, false, &makeLoader<GltfModelLoader>},
        {"obj", {"obj"}, 30, false, &makeLoader<ObjModelLoader>},
        // Saba Model/ migrations: PMX / PMD / XFile / VMD.  All four carry a
        // magic number, so they win dispatch by content even when the file is
        // renamed or has no extension.
        {"pmx", {"pmx"}, 10, true, &makeLoader<PmxModelLoader>},
        {"pmd", {"pmd"}, 10, true, &makeLoader<PmdModelLoader>},
        {"xfile", {"x"}, 10, true, &makeLoader<XFileModelLoader>},
        {"vmd", {"vmd"}, 10, true, &makeLoader<VmdAnimationLoader>},
    };
    return rows;
}

} // namespace

void registerBuiltinModelLoaders(ModelLoaderRegistry &registry)
{
    for (const BuiltinRow &row : builtinRows()) {
        ModelLoaderDescriptor descriptor;
        descriptor.name = row.name;
        descriptor.extensions = row.extensions;
        descriptor.priority = row.priority;
        descriptor.magicProbe = row.magicProbe;
        descriptor.factory = row.factory;
        registry.registerLoader(std::move(descriptor));
    }
}
