#pragma once
#include "ModelLoader.h"

// MikuMikuDance PMX 2.0/2.1 loader.  Migrated from Saba's
// Model/MMD/PMXFile.cpp, but re-expressed as an IModelLoader strategy that
// emits the loader-neutral Model: positions/normals/UVs go into the shared
// vertex buffer, BDEF*/SDEF/QDEF weights into Model::skins + the parallel
// boneIndices/boneWeights arrays, and the bone tree into Model::nodes.
class PmxModelLoader : public IModelLoader {
public:
    const char *name() const override { return "pmx"; }
    std::vector<std::string> extensions() const override { return {"pmx"}; }
    Model load(const std::string &path, const ModelLoadOptions &options) const override;
    int probe(const uint8_t *data, size_t size) const override;
    PostProcessPlan defaultPlan(const std::string &path) const override;
    void postProcess(Model &model, const ModelLoadOptions &options) const override;
};
