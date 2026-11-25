#pragma once
#include "ModelLoader.h"

#include <string>
#include <vector>

// Assimp-backed loader: one implementation for every format assimp can read
// (.dae, .fbx, .3ds, .ply, ...), which is what the framework's "add a format,
// don't touch the consumers" claim needed to be tested against.
//
// It absorbs the loading method from LearnOpenGL's includes/learnopengl/model.h
// (node recursion, per-mesh material, the diffuse/specular/normal/height texture
// slots) with three deliberate differences, all of them consequences of the
// engine not being OpenGL:
//
//   * node transforms are applied.  The reference walks the hierarchy but drops
//     every aiNode::mTransformation, which silently flattens hierarchical files;
//     here each node keeps its local matrix and the examples use
//     computeNodeWorldMatrices / forEachMeshInstance.
//   * no GenSmoothNormals / CalcTangentSpace / FlipUVs flags.  Each has an exact
//     equivalent in the shared post-process chain, so asking assimp for them too
//     would both duplicate the work and let the two implementations disagree
//     about the same vertex.
//   * the .dll is bound at run time.  The registry is linked into every example,
//     so a link-time import would make all of them fail to start when assimp is
//     absent -- for a loader that only some examples use.
class AssimpModelLoader : public IModelLoader {
public:
    const char *name() const override { return "assimp"; }
    std::vector<std::string> extensions() const override;
    Model load(const std::string &path, const ModelLoadOptions &options) const override;

    // V axis: assimp hands out the file's own convention, which for OBJ/DAE/FBX
    // is bottom-left (same as aiProcess_FlipUVs exists to fix).  glTF is the
    // exception and is already top-left, matching Vulkan's sampled-image origin.
    PostProcessPlan defaultPlan(const std::string &path) const override;

    // Empty string when the importer is usable; otherwise the reason, suitable
    // for appending to a "no loader for this file" message.
    static std::string runtimeDiagnostic();

private:
    static bool isTopLeftV(const std::string &path);
};
