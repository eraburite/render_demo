#pragma once
#include "ModelLoader.h"

// Vocaloid Motion Data loader, migrated from Saba's Model/MMD/VMDFile.cpp.
// VMD is animation-only: it registers as an IModelLoader so it shares the same
// plugin table and magic-probe dispatch, but load() is intentionally rejected
// in favour of loadModelAnimation().
class VmdAnimationLoader : public IModelLoader {
public:
    const char *name() const override { return "vmd"; }
    std::vector<std::string> extensions() const override { return {"vmd"}; }
    bool isAnimationOnly() const override { return true; }
    Model load(const std::string &path, const ModelLoadOptions &options) const override;
    AnimationClip loadAnimation(const std::string &path) const override;
    int probe(const uint8_t *data, size_t size) const override;
};
