#pragma once
#include "ModelLoader.h"

// MikuMikuDance PMD (the predecessor of PMX) loader, migrated from Saba's
// Model/MMD/PMDFile.cpp.  Fixed-width Shift-JIS fields, 2-bone vertex weights
// and Phong materials all map onto the same Model as PMX.
class PmdModelLoader : public IModelLoader {
public:
    const char *name() const override { return "pmd"; }
    std::vector<std::string> extensions() const override { return {"pmd"}; }
    Model load(const std::string &path, const ModelLoadOptions &options) const override;
    int probe(const uint8_t *data, size_t size) const override;
    PostProcessPlan defaultPlan(const std::string &path) const override;
    void postProcess(Model &model, const ModelLoadOptions &options) const override;
};
