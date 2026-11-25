#pragma once
#include "ModelLoader.h"

// glTF 2.0 loader (.gltf / .glb) backed by tinygltf.
//
// The compiled tinygltf + stb_image implementations live in the matching .cpp;
// this header is safe to include from anywhere.
class GltfModelLoader : public IModelLoader {
public:
    const char *name() const override { return "gltf"; }
    std::vector<std::string> extensions() const override { return {"gltf", "glb"}; }
    Model load(const std::string &path, const ModelLoadOptions &options) const override;

    // glTF primitives are already indexed and carry TANGENT in most PBR
    // assets, so the default plan asks for both fix-ups: they are no-ops when
    // the attribute is present and rescue the file when it is not.
    PostProcessPlan defaultPlan(const std::string &) const override
    {
        PostProcessPlan plan;
        plan.deduplicate = true;
        plan.generateNormals = true;
        plan.generateTangents = true;
        return plan;
    }
};
