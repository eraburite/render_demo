#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

// CPU-side glTF loader.  It deliberately stops at plain data: the examples
// decide how to upload vertices/images and how to lay out descriptor sets.
// This keeps the engine free of any glTF/tinygltf dependency.

// Single vertex format shared by all glTF examples.  Every primitive is
// expanded into one global vertex/index buffer so a draw is just
// (firstIndex, indexCount) plus a material id.
struct GlTFVertex {
    glm::vec3 position{};
    glm::vec3 normal{};
    glm::vec2 texCoord{};
    glm::vec3 tangent{};
    glm::vec3 color{1.0f, 1.0f, 1.0f};
};

struct GlTFMaterial {
    std::string name;
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff = 0.5f;
    int alphaMode = 0; // 0 = OPAQUE, 1 = MASK, 2 = BLEND
    bool doubleSided = false;
    int baseColorTexture = -1;
    int normalTexture = -1;
    int metallicRoughnessTexture = -1;
    int occlusionTexture = -1;
    int emissiveTexture = -1;
};

struct GlTFPrimitive {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int materialIndex = -1;
};

struct GlTFMesh {
    std::string name;
    std::vector<GlTFPrimitive> primitives;
};

struct GlTFNode {
    std::string name;
    glm::mat4 transform{1.0f}; // local matrix (TRS or explicit)
    int meshIndex = -1;
    std::vector<int> children;
};

struct GlTFImage {
    std::vector<uint8_t> pixels; // RGBA8
    int width = 0;
    int height = 0;
    uint32_t mipLevels = 1;
};

struct GlTFModel {
    std::vector<GlTFVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<GlTFMesh> meshes;
    std::vector<GlTFNode> nodes;
    std::vector<int> rootNodes;
    std::vector<GlTFMaterial> materials;
    std::vector<GlTFImage> images;
    glm::vec3 minBounds{0.0f};
    glm::vec3 maxBounds{0.0f};

    bool valid() const { return !vertices.empty() && !indices.empty(); }
};

// Loads an ASCII (.gltf) or binary (.glb) file.  Throws std::runtime_error with
// a readable message on malformed input.  Textures are decoded to RGBA8 via
// stb_image; missing optional attributes are filled with safe defaults.
GlTFModel loadGlTF(const std::string &path);

// Recursively computes world matrices for every node.  Returned vector is
// parallel to model.nodes.
std::vector<glm::mat4> computeNodeWorldMatrices(const GlTFModel &model);
