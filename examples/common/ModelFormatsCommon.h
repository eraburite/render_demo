#pragma once
#include "ModelAsset.h"
#include "ModelLoader.h"
#include "TextureLoader.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Conventions shared by the MMD formats (PMX / PMD / VMD) and DirectX X.
//
// A loader's job is to decode bytes; the *semantics* of an MMD material belong
// here so that PMX and PMD cannot drift apart.  finalizeMaterials() is invoked
// from IModelLoader::postProcess (the Template Method hook), i.e. after the
// shared pipeline, where it can safely touch anything the chain produced.
// ---------------------------------------------------------------------------
namespace modelformats {

inline std::string tagMaterial(const std::string &name) { return "phong:" + name; }

inline bool isPhongMaterial(const Material &material)
{
    return material.name.rfind("phong:", 0) == 0;
}

inline Material makePhongMaterial(const std::string &name, const glm::vec4 &diffuse,
                                  const glm::vec3 &specular, float shininess, bool doubleSided)
{
    Material material;
    material.name = tagMaterial(name);
    material.baseColorFactor = diffuse;
    material.emissiveFactor = glm::vec3(0.0f);
    material.metallicFactor = 0.0f;   // Phong / dielectric; overridden in finalize
    material.roughnessFactor = 1.0f;  // derived from shininess in finalize
    material.shininess = shininess;
    material.doubleSided = doubleSided;
    material.alphaMode = diffuse.a < 1.0f ? AlphaMode::eBlend : AlphaMode::eOpaque;
    (void)specular; // folded into the PBR approximation below
    return material;
}

inline void finalizeMaterials(Model &model)
{
    for (Material &material : model.materials) {
        if (!isPhongMaterial(material)) continue;
        // MMD draws both faces by convention (toon outlines rely on it), and
        // its diffuse alpha -- not glTF's alphaMode -- decides blending.
        material.doubleSided = true;
        material.alphaMode = material.baseColorFactor.a < 1.0f ? AlphaMode::eBlend : AlphaMode::eOpaque;
        // Phong shininess -> a PBR-roughness approximation:
        //   roughness ~ sqrt(2 / (shininess + 2))
        // so a hard specular (20) becomes ~0.30 and a matte one (2) ~0.71.
        const float shininess = std::max(material.shininess, 0.0f);
        material.roughnessFactor = std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.05f, 1.0f);
        material.metallicFactor = 0.0f;
    }
}

// Resolve a side-car texture relative to the model file.  A missing or
// undecodable texture is not fatal: MMD models routinely reference toon/sphere
// files that are not shipped with the model.  Returns -1 when unavailable.
inline int loadSideCarTexture(Model &model, const std::string &baseDirectory, const std::string &name)
{
    if (name.empty()) return -1;
    std::string path = name;
    if (!baseDirectory.empty()) path = baseDirectory + "/" + name;
    {
        std::ifstream probe(path, std::ios::binary);
        if (!probe) return -1;
    }
    try {
        const LoadedTexture loaded = loadTexture(path);
        if (loaded.pixels.empty() || loaded.width <= 0 || loaded.height <= 0) return -1;
        TextureImage image;
        image.pixels = loaded.pixels;
        image.width = loaded.width;
        image.height = loaded.height;
        image.mipLevels = loaded.mipLevels;
        image.name = name;
        model.images.push_back(std::move(image));
        return static_cast<int>(model.images.size()) - 1;
    } catch (...) {
        return -1;
    }
}

inline std::string directoryOf(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline std::string baseDirectoryFor(const std::string &path, const ModelLoadOptions &options)
{
    return options.baseDirectory.empty() ? directoryOf(path) : options.baseDirectory;
}

// Resolve name-addressed animation channels (VMD) against a loaded Model.
// Returns the number of channels that found a node.  A channel that already
// carries a valid nodeIndex is left untouched, so the same helper is safe to
// call on glTF/assimp clips.
inline uint32_t bindAnimationChannelsToModel(AnimationClip &clip, const Model &model)
{
    uint32_t bound = 0;
    for (AnimationChannel &channel : clip.channels) {
        if (channel.nodeIndex >= 0 && static_cast<size_t>(channel.nodeIndex) < model.nodes.size()) {
            ++bound;
            continue;
        }
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (model.nodes[i].name == channel.nodeName) {
                channel.nodeIndex = static_cast<int>(i);
                ++bound;
                break;
            }
        }
    }
    return bound;
}

} // namespace modelformats
