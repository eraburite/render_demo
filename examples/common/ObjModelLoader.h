#pragma once
#include "ModelLoader.h"

// Wavefront OBJ loader (.obj + .mtl) backed by tiny_obj_loader.
//
// This is the second format in the tree and exists mostly as proof that the
// abstraction holds: it implements the same IModelLoader interface, produces
// the same Model structure, and gets dedup / normal / tangent / transform
// fix-ups from the shared post-process chain without a single extra line.
class ObjModelLoader : public IModelLoader {
public:
    const char *name() const override { return "obj"; }
    std::vector<std::string> extensions() const override { return {"obj"}; }
    Model load(const std::string &path, const ModelLoadOptions &options) const override;

    PostProcessPlan defaultPlan(const std::string &) const override
    {
        PostProcessPlan plan;
        plan.deduplicate = true;
        // OBJ/MTL keeps texel (0,0) at the bottom-left; the engine samples with
        // Vulkan's top-left origin, so the V axis has to be mirrored once.
        plan.flipTexCoordV = true;
        plan.generateNormals = true;
        plan.generateTangents = true;
        return plan;
    }
};
