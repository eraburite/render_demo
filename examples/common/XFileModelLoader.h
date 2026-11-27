#pragma once
#include "ModelLoader.h"

// DirectX X file (.x, text flavour) loader, migrated from Saba's
// Model/XFile/XFileModel.cpp (which used tinyxfileloader).  Frame /
// FrameTransformMatrix become Model::nodes, and Mesh / MeshNormals /
// MeshTextureCoords / MeshMaterialList become meshes with per-material
// primitive windows.
class XFileModelLoader : public IModelLoader {
public:
    const char *name() const override { return "xfile"; }
    std::vector<std::string> extensions() const override { return {"x"}; }
    Model load(const std::string &path, const ModelLoadOptions &options) const override;
    int probe(const uint8_t *data, size_t size) const override;
    PostProcessPlan defaultPlan(const std::string &path) const override;
    void postProcess(Model &model, const ModelLoadOptions &options) const override;
};
